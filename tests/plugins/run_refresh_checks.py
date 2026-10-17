"""Run real GDI refresh checks on a private desktop (Windows + pywin32).

Usage: python run_refresh_checks.py plugin_abi_tests.exe lifecycle.dll NEW_OUTPUT_DIRECTORY
The desktop is never activated. Only this runner's child is terminated on timeout.
"""
import json
import msvcrt
import os
from pathlib import Path
import shutil
import subprocess
import sys

import win32api
import win32con
import win32event
import win32process
import win32service


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    binary, lifecycle, output = (Path(value).resolve() for value in sys.argv[1:])
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if not lifecycle.is_file():
        raise FileNotFoundError(lifecycle)
    output.mkdir(exist_ok=False)
    target = output / binary.name
    shutil.copy2(binary, target)
    desktop_name = 'MMS_REFRESH_TEST_' + str(os.getpid())
    desktop = win32service.CreateDesktop(desktop_name, 0, win32con.GENERIC_ALL, None)
    code = None
    try:
        with (output / 'result.log').open('wb') as log, open(os.devnull, 'rb') as empty:
            startup = win32process.STARTUPINFO()
            startup.lpDesktop = 'winsta0\\' + desktop_name
            startup.dwFlags = win32con.STARTF_USESHOWWINDOW | win32con.STARTF_USESTDHANDLES
            startup.wShowWindow = win32con.SW_HIDE
            startup.hStdOutput = startup.hStdError = msvcrt.get_osfhandle(log.fileno())
            startup.hStdInput = msvcrt.get_osfhandle(empty.fileno())
            for handle in (startup.hStdOutput, startup.hStdInput):
                os.set_handle_inheritable(handle, True)
            env = os.environ.copy()
            (output / 'local').mkdir()
            (output / 'temp').mkdir()
            env.update(LOCALAPPDATA=str(output / 'local'), TEMP=str(output / 'temp'), TMP=str(output / 'temp'),
                       MMS_ABI_PROBE_LOG=str(output / 'plugin.log'))
            process, thread, _, _ = win32process.CreateProcess(
                str(target), subprocess.list2cmdline([str(target), '--refresh-only', str(lifecycle)]),
                None, None, True, win32con.CREATE_NO_WINDOW, env, str(output), startup)
            try:
                if win32event.WaitForSingleObject(process, 180000) != win32con.WAIT_OBJECT_0:
                    win32api.TerminateProcess(process, 124)
                    win32event.WaitForSingleObject(process, 5000)
                    raise TimeoutError('refresh checks timed out')
                code = win32process.GetExitCodeProcess(process)
            finally:
                thread.Close()
                process.Close()
    finally:
        desktop.CloseDesktop()
    print((output / 'result.log').read_text(encoding='utf-8', errors='replace'))
    (output / 'report.json').write_text(json.dumps(
        dict(binary=str(binary), desktop=desktop_name, exitCode=code), indent=2), encoding='utf-8')
    return code


if __name__ == '__main__':
    sys.exit(main())
