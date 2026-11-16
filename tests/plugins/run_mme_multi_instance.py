"""Check real MMS instances sharing one MME cache and preferences directory."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import psutil


USER = c.WinDLL("user32", use_last_error=True)
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
USER.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
USER.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
USER.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
USER.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
USER.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]


def windows(pid):
    result = []

    @CALLBACK
    def visit(window, unused):
        owner = w.DWORD()
        USER.GetWindowThreadProcessId(window, c.byref(owner))
        if owner.value == pid:
            title, kind = c.create_unicode_buffer(2048), c.create_unicode_buffer(128)
            USER.GetWindowTextW(window, title, len(title))
            USER.GetClassNameW(window, kind, len(kind))
            result.append((window, kind.value, title.value))
        return True

    USER.EnumWindows(visit, 0)
    return result


def wait_ready(processes, directory):
    deadline = time.monotonic() + 45
    stable = None
    while time.monotonic() < deadline:
        ready = True
        for index, process in enumerate(processes):
            if process.poll() is not None:
                raise RuntimeError(f"Instance {index} exited: {process.returncode}")
            current = windows(process.pid)
            dialogs = [title for _, kind, title in current if kind == "#32770"]
            if dialogs:
                raise RuntimeError(f"Instance {index} showed dialogs: {dialogs}")
            main = [h for h, kind, _ in current if kind == "Polygon Movie Maker"]
            for window in main:
                # Keep only the test's own windows outside the user's desktop.
                USER.SetWindowPos(window, None, -10000, -10000, 1200, 900, 0x14)
            trace = directory / f"trace-{index}.log"
            ready &= bool(main) and trace.exists() and "MME_BACKEND" in trace.read_text(errors="replace")
        if ready:
            stable = stable or time.monotonic()
            if time.monotonic() - stable >= 2:
                return
        else:
            stable = None
        time.sleep(.1)
    raise RuntimeError("Timed out waiting for all MMS instances")


def check_case(args, directory, simultaneous):
    directory.mkdir()
    for source in args.runtime.glob("*.dll"):
        shutil.copy2(source, directory / source.name)
    executable = directory / "MikuMikuStudio.exe"
    shutil.copy2(args.runtime / executable.name, executable)
    shutil.copytree(args.runtime / "PLUGINS", directory / "PLUGINS")
    package = directory / "PLUGINS/LEGACY/mme"
    package.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.mme, package / "MMEffect.dll")
    processes, modules = [], []
    settings_before = None
    try:
        for index in range(2):
            env = os.environ.copy()
            env.update(LOCALAPPDATA=str(directory / "Local"), APPDATA=str(directory / "Roaming"),
                       MIKUMIKUSTUDIO_TRACE_FILE=str(directory / f"trace-{index}.log"))
            processes.append(subprocess.Popen([str(executable)], cwd=directory, env=env))
            if not simultaneous:
                wait_ready(processes, directory)
                if index == 0:
                    settings = list((directory / "data/plugins/legacy_mme/settings").glob("*/MMEffect.ini"))
                    if len(settings) != 1:
                        raise RuntimeError("Expected one stable preferences file")
                    with settings[0].open("ab") as file:
                        file.write(b"\n;multi-instance-preserved\n")
                    settings_before = settings[0].read_bytes()
        wait_ready(processes, directory)
        for process in processes:
            loaded = {Path(m.path).name.lower(): m.path for m in psutil.Process(process.pid).memory_maps()
                      if Path(m.path).name.lower() in ("mmhack.dll", "mmeffect.dll", "mmdfacade.dll")}
            if len(loaded) != 3:
                raise RuntimeError(f"Missing MME modules: {loaded}")
            modules.append(loaded)
        if modules[0] != modules[1]:
            raise RuntimeError("Instances did not share the same verified DLL cache")
        settings = list((directory / "data/plugins/legacy_mme/settings").glob("*/MMEffect.ini"))
        if len(settings) != 1 or not settings[0].read_bytes():
            raise RuntimeError("Preferences initialization was incomplete")
        if settings_before is not None and settings[0].read_bytes() != settings_before:
            raise RuntimeError("Second instance overwrote existing preferences")
        return dict(passed=True, modules=modules, settings=str(settings[0]),
                    executableSha256=hashlib.sha256(executable.read_bytes()).hexdigest())
    finally:
        for process in processes:
            for window, kind, _ in windows(process.pid):
                if kind == "#32770":
                    USER.PostMessageW(window, 0x111, 1, 0)
                elif kind == "Polygon Movie Maker":
                    USER.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--mme", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="mme-multi-", dir=args.output.resolve()))
    report = {}
    for name, simultaneous in (("second-instance", False), ("simultaneous-cold-start", True)):
        try:
            report[name] = check_case(args, root / name, simultaneous)
        except Exception as error:
            report[name] = dict(passed=False, error=str(error))
        print(name, report[name], flush=True)
    (root / "result.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(root / "result.json", flush=True)
    return int(not all(case["passed"] for case in report.values()))


if __name__ == "__main__":
    raise SystemExit(main())
