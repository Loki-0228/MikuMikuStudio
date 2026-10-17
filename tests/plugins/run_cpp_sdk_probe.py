"""Exercise the SDK hello DLL in isolated hidden production hosts."""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from host_smoke import children, send, u, windows

u.GetMenuState.argtypes = [w.HMENU, w.UINT, w.UINT]
u.GetMenuState.restype = w.UINT


def menu_command(main):
    send(main, 0x116, 0, 0)  # WM_INITMENU asks the host to refresh command state.
    root = u.GetMenu(main)
    for position in range(u.GetMenuItemCount(root)):
        menu = u.GetSubMenu(root, position)
        if menu:
            for item in range(u.GetMenuItemCount(menu)):
                command = u.GetMenuItemID(menu, item)
                if 0xA000 <= command < 0xC000:
                    return menu, command
    return None, None


def checked(main, expected):
    menu, command = menu_command(main)
    assert menu and command is not None, "SDK command absent from production menu"
    state = u.GetMenuState(menu, command, 0)
    assert state != 0xFFFFFFFF and bool(state & 8) == expected, (state, expected)
    return command


def run(binary, plugin, output, name):
    output.mkdir(parents=True, exist_ok=False)
    target = output / name
    shutil.copy2(binary, target)
    package = output / "PLUGINS" / "开发示例" / "声明式SDK"
    package.mkdir(parents=True)
    shutil.copy2(plugin, package / "hello_menu.dll")
    shutil.copy2(plugin.parent / "plugin.json", package / "plugin.json")
    for folder in ("Data", "local", "roaming", "temp"):
        (output / folder).mkdir()
    environment = os.environ.copy()
    environment.update(LOCALAPPDATA=str(output / "local"), APPDATA=str(output / "roaming"),
                       TEMP=str(output / "temp"), TMP=str(output / "temp"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    report = {"name": name, "completed": False}
    with (output / "result.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(target)], cwd=output, env=environment, startupinfo=startup,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            main = toolbar = None
            buttons = []
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"host exited during initialization: {process.returncode}")
                main = next((h for h, cls in windows(process.pid) if cls == "Polygon Movie Maker"), None)
                if main:
                    menu, command = menu_command(main)
                    toolbar = next((h for h, cls in children(main) if cls == "MMS.PluginAbi.Toolbar.V1"), None)
                    buttons = [h for h, cls in children(toolbar) if cls == "Button"] if toolbar else []
                    if menu and buttons:
                        break
                time.sleep(.05)
            else:
                raise TimeoutError("SDK menu/button registration did not become available")
            assert not u.IsWindowVisible(main), "test host became visible"
            command = checked(main, False)
            send(main, 0x111, command, 0)
            checked(main, True)
            send(toolbar, 0x111, u.GetDlgCtrlID(buttons[0]), buttons[0])
            checked(main, False)
            # Repeat after the native host has recomputed all layout geometry.
            send(main, 0x5, 0, (700 << 16) | 920)
            command = checked(main, False)
            send(main, 0x111, command, 0)
            checked(main, True)
            send(toolbar, 0x111, u.GetDlgCtrlID(buttons[0]), buttons[0])
            checked(main, False)
            send(main, 0x10)
            process.wait(timeout=15)
            assert process.returncode == 0, process.returncode
            report.update(completed=True, exitCode=process.returncode, invocations=4)
        except Exception as error:
            report["error"] = str(error)
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            report["exitCode"] = process.returncode
            (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"PASS {name}: SDK DLL menu/button share state before and after layout; clean shutdown", flush=True)
    return report


def main():
    binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    report = {"binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "pluginSha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "cases": []}
    for folder, name in (("original", "MikuMikuDance.exe"), ("renamed", "自定义SDK宿主.exe")):
        report["cases"].append(run(binary, plugin, output / folder, name))
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
