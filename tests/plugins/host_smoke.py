"""Start only an isolated, hidden MMS copy; exercise production UI dispatch.

All configuration and plugin data stay in the supplied scratch directory.
The user's running editor and installed plugins are never addressed.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

u = c.WinDLL("user32", use_last_error=True)
EnumProc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [EnumProc, w.LPARAM]
u.EnumChildWindows.argtypes = [w.HWND, EnumProc, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.IsWindowVisible.argtypes = [w.HWND]
u.GetMenu.argtypes = [w.HWND]
u.GetMenu.restype = w.HMENU
u.GetSubMenu.argtypes = [w.HMENU, c.c_int]
u.GetSubMenu.restype = w.HMENU
u.GetMenuItemCount.argtypes = [w.HMENU]
u.GetMenuItemID.argtypes = [w.HMENU, c.c_int]
u.GetMenuItemID.restype = w.UINT
u.GetDlgCtrlID.argtypes = [w.HWND]
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
u.SendMessageTimeoutW.restype = w.LPARAM
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetParent.argtypes = [w.HWND]
u.GetParent.restype = w.HWND
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.GetWindowTextLengthW.argtypes = [w.HWND]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]

u.WaitForInputIdle.argtypes = [w.HANDLE, w.DWORD]


def send(hwnd, message, wp=0, lp=0):
    result = c.c_size_t()
    if not u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 5000, c.byref(result)):
        raise RuntimeError(f"message {message:#x} timed out / failed {c.get_last_error()}")
    return result.value


def windows(pid):
    result = []

    @EnumProc
    def visit(hwnd, _):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            name = c.create_unicode_buffer(256)
            u.GetClassNameW(hwnd, name, 256)
            result.append((hwnd, name.value))
        return True

    u.EnumWindows(visit, 0)
    return result


def children(parent):
    result = []

    @EnumProc
    def visit(hwnd, _):
        name = c.create_unicode_buffer(256)
        u.GetClassNameW(hwnd, name, 256)
        result.append((hwnd, name.value))
        return True

    u.EnumChildWindows(parent, visit, 0)
    return result


def run(executable, fixture, directory, filename, mode="gui"):
    directory.mkdir(parents=True, exist_ok=False)
    target = directory / filename
    shutil.copy2(executable, target)
    package = directory / "PLUGINS" / "界面" / "测试"
    package.mkdir(parents=True)
    shutil.copy2(fixture, package / "lifecycle.dll")
    manifest = dict(schemaVersion=1, id="org.example." + mode, name="ABI GUI test", version="1.0.0", kind="native",
                    abi=dict(major=1, minMinor=0), entry="lifecycle.dll", architecture="x64", minWindows="6.1.7601",
                    capabilities=["ui.command", "ui.element"], requiresServices=[dict(id="mms.ui", major=1, minMinor=0)],
                    dependencies=[], nativeDependencies=[], reloadPolicy="restart")
    (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
    (directory / "Data").mkdir()
    environment = os.environ.copy()
    environment["LOCALAPPDATA"] = str(directory / "userdata")
    environment["MMS_ABI_PROBE_LOG"] = str(directory / "probe.log")
    environment["MMS_UI_METRICS"] = str(directory / "ui-metrics.jsonl")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(target)], cwd=directory, env=environment, startupinfo=startup)
    main = None
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"host exited early {process.returncode}")
            main = next((h for h, cls in windows(process.pid) if cls == "Polygon Movie Maker"), None)
            if main and (directory / "probe.log").exists() and "GUI_REGISTERED" in (directory / "probe.log").read_text():
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"initialization timeout; process windows={windows(process.pid)}")
        assert not u.IsWindowVisible(main), "host was shown"
        idle = u.WaitForInputIdle(w.HANDLE(int(process._handle)), 30000)
        if idle:
            raise RuntimeError(f"host not idle after startup: {idle}; windows={windows(process.pid)}")
        # Wait until queued registry publication has mounted both controls.
        deadline = time.monotonic() + 5
        toolbar = None
        while time.monotonic() < deadline:
            send(main, 0x116, 0, 0)  # WM_INITMENU, production refresh path
            toolbar = next((h for h, cls in children(main) if cls == "MMS.PluginAbi.Toolbar.V1"), None)
            buttons = [h for h, cls in children(toolbar) if cls == "Button"] if toolbar else []
            if buttons:
                break
            time.sleep(0.05)
        assert toolbar and buttons, "toolbar button absent"
        bar = u.GetMenu(main)
        command = None
        for pos in range(u.GetMenuItemCount(bar)):
            menu = u.GetSubMenu(bar, pos)
            if menu:
                for item in range(u.GetMenuItemCount(menu)):
                    candidate = u.GetMenuItemID(menu, item)
                    if 0xa000 <= candidate < 0xc000:
                        command = candidate
                        break
        assert command is not None, "new plugin command absent"
        send(main, 0x111, command)  # WM_COMMAND
        send(toolbar, 0x111, u.GetDlgCtrlID(buttons[0]), buttons[0])
        log = (directory / "probe.log").read_text()
        assert log.count("COMMAND_INVOKED") == 2, log
        if "refresh-auto-release" in mode:
            assert log.count("REFRESH_TARGET_IN_COMMAND") == 2 and log.count("REFRESH_WORKER_PENDING") == 2, log
            assert log.count("REFRESH_RELEASED") == 2 and "REFRESH_WORKER_FAILED" not in log and "REFRESH_RELEASE_FAILED" not in log, log
        if "scene" in mode:
            assert log.count("SCENE_READ_PASS") == 2, log
        rect = w.RECT()
        u.GetWindowRect(toolbar, c.byref(rect))
        assert rect.bottom > rect.top, "toolbar reserved no space"
        if "panel" in mode:
            log = (directory / "probe.log").read_text()
            assert log.count("SURFACE_CREATE") == 1 and "SURFACE_SHOW" in log, log
            tab = next((h for h, cls in children(main) if cls == "SysTabControl32"), None)
            assert tab, "panel tabs absent"
            send(tab, 0x1330, 1)  # TCM_SETCURFOCUS, no cross-process pointers
            log = (directory / "probe.log").read_text()
            assert log.count("SURFACE_CREATE") == 2 and "SURFACE_HIDE" in log and "SURFACE_FOCUS" in log, log
            send(tab, 0x1330, 0)
            assert (directory / "probe.log").read_text().count("SURFACE_CREATE") == 2, "switch recreated a live view"
            widths = []
            for host_width in (1200, 960, 1400):
                assert u.SetWindowPos(main, None, 0, 0, host_width, 820, 0x16)
                inputs = [h for h, cls in children(main) if cls.lower() == "edit" and u.GetDlgCtrlID(h) == 0x7101]
                assert len(inputs) == 2, "both native panel inputs remain alive"
                for edit in inputs:
                    panel = u.GetParent(edit)
                    assert u.GetWindowTextLengthW(panel) == 0, "panel container must not paint text underneath input"
                    client, rect, origin = w.RECT(), w.RECT(), w.POINT()
                    assert u.GetClientRect(panel, c.byref(client)) and u.GetWindowRect(edit, c.byref(rect))
                    assert u.ClientToScreen(panel, c.byref(origin))
                    assert rect.left - origin.x == 8 and rect.top - origin.y == 8
                    assert rect.right - origin.x == client.right - 8, "input follows panel width without clipping"
                    assert rect.bottom - rect.top == 24
                    text = c.create_unicode_buffer(64)
                    u.GetWindowTextW(edit, text, len(text))
                    assert text.value == "中文输入测试", "resizing retains input contents"
                widths.append(rect.right - rect.left)
            assert widths[1] < widths[0] < widths[2], "input adapts when the production host shrinks and grows"
            client, origin, toolbar_rect = w.RECT(), w.POINT(), w.RECT()
            u.GetClientRect(main, c.byref(client));u.ClientToScreen(main, c.byref(origin));u.GetWindowRect(toolbar,c.byref(toolbar_rect))
            sidebar = toolbar_rect.left - origin.x - 9
            send(main, 0x201, 1, ((100 & 0xffff) << 16) | ((sidebar+3) & 0xffff))
            for target in (400, 600, 300):
                send(main, 0x200, 1, (100 << 16) | target)
                mounts = [h for h, cls in children(main) if cls in ("MMS.PluginAbi.Toolbar.V1", "MMS.PluginAbi.Workspace.V1") and u.GetParent(h) == main]
                assert len(mounts) == 2
                for mount in mounts:
                    rect = w.RECT();u.GetWindowRect(mount,c.byref(rect))
                    assert rect.left-origin.x == target+9, "plugin mount follows live sidebar before release"
                    assert rect.right-origin.x == client.right, "plugin mount uses remaining client width"
            send(main, 0x202, 0, (100 << 16) | 300)
            print("PASS plugin toolbar and workspace follow each live sidebar position", flush=True)


        send(main, 0x10)  # WM_CLOSE, no document was modified
        process.wait(timeout=15)
        assert process.returncode == 0, process.returncode
        log = (directory / "probe.log").read_text()
        assert "UI_CLOSING" in log and log.splitlines().count("STOP_OK") == 1 and log.splitlines().count("DESTROY") == 1, log
        if "panel" in mode:
            assert log.count("SURFACE_DESTROY") == 2 and log.index("SURFACE_DESTROY") < log.index("STOP_OK"), log
        metrics = json.loads((directory / "ui-metrics.jsonl").read_text().splitlines()[-1])
        assert metrics["layout"] > 0 and all(isinstance(v, int) and v >= 0 for v in metrics.values()), metrics
        print(f"PASS {filename}: hidden production startup, menu, toolbar, close; toolbar height={rect.bottom - rect.top}", flush=True)
    except Exception:
        print(f"FAILED {filename}; windows={windows(process.pid)}", flush=True)
        if (directory / "probe.log").exists():
            print((directory / "probe.log").read_text(), flush=True)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    exe, dll, output = map(lambda x: Path(x).resolve(), sys.argv[1:4])
    mode = sys.argv[4] if len(sys.argv) > 4 else "gui"
    for name, filename in [("normal", "MikuMikuDance.exe"), ("renamed", "舞台编辑器.exe")]:
        run(exe, dll, output / name, filename, mode)
