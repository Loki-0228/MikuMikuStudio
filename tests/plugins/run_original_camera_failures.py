"""Assert actual startup error dialogs, and silent deliberate disable, with the original input."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from run_original_camera import SHA
import hashlib
import ctypes as c

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--original', type=Path, required=True)
args = parser.parse_args()
build, original = args.build.resolve(), args.original.resolve()
assert hashlib.sha256(original.read_bytes()).hexdigest() == SHA
root = Path(tempfile.mkdtemp(prefix='original-camera-failures-', dir=build.parent))
cases = []
for mode in ['missing-provider', 'wrong-provider', 'locked-file', 'unsupported-version', 'duplicate', 'ambiguous-provider', 'disabled']:
    directory = root / mode
    package = directory / 'PLUGINS/LEGACY/camera'
    package.mkdir(parents=True)
    dll = package / original.name
    shutil.copy2(original, dll)
    exe = directory / '中文测试.exe'
    shutil.copy2(build / 'tests/Release/original_camera_input_tests.exe', exe)
    shutil.copy2(build / 'Release/MMDFacade.dll', directory / 'MMDFacade.dll')
    if mode != 'missing-provider':
        for path in (['PLUGINS/components/a', 'PLUGINS/components/b'] if mode == 'ambiguous-provider' else ['PLUGINS/LEGACY']):
            provider = directory / path / 'MMDPlugin.dll'
            provider.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / 'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
            if mode == 'wrong-provider': shutil.copy2(original, provider)
    if mode == 'unsupported-version':
        data = bytearray(dll.read_bytes()); data[-1] ^= 1; dll.write_bytes(data)
    if mode == 'duplicate':
        second = directory / 'PLUGINS/LEGACY/second'
        second.mkdir(); shutil.copy2(original, second / original.name)
    if mode == 'disabled':
        (directory / 'PLUGINS/config.json').write_text(json.dumps(dict(schemaVersion=1, maxDepth=2,
            disabledPlugins=['org.mikumikustudio.legacy.original-first-person-camera'])), encoding='utf-8')
    environment = os.environ.copy()
    environment['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
    lock = None
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [c.c_wchar_p, c.c_uint, c.c_uint, c.c_void_p, c.c_uint, c.c_uint, c.c_void_p]
    kernel.CreateFileW.restype = c.c_void_p
    kernel.CloseHandle.argtypes = [c.c_void_p]
    if mode == 'locked-file':
        lock = kernel.CreateFileW(str(dll), 0x80000000, 0, None, 3, 0x80, None)
        if lock in (None, c.c_void_p(-1).value): raise c.WinError(c.get_last_error())
    try:
        run = subprocess.run([str(exe), mode], cwd=directory, env=environment, capture_output=True,
            timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    finally:
        if lock: kernel.CloseHandle(lock)
    (directory / 'result.log').write_bytes(run.stdout + run.stderr)
    cases.append(dict(mode=mode, passed=run.returncode == 0, exitCode=run.returncode))
    print(cases[-1], flush=True)
    (root / 'summary.json').write_text(json.dumps(cases, indent=2), encoding='utf-8')
    if run.returncode: raise SystemExit(1)
print(root / 'summary.json')
