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
u.GetWindowLongPtrW.argtypes = [w.HWND, c.c_int]
u.GetWindowLongPtrW.restype = c.c_ssize_t
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.IsWindow.argtypes = [w.HWND]
u.OpenDesktopW.argtypes = [w.LPCWSTR, w.DWORD, w.BOOL, w.DWORD]
u.OpenDesktopW.restype = w.HANDLE
u.CloseDesktop.argtypes = [w.HANDLE]
u.EnumDesktopWindows.argtypes = [w.HANDLE, EnumProc, w.LPARAM]
u.GetThreadDesktop.argtypes = [w.DWORD]
u.GetThreadDesktop.restype = w.HANDLE
u.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
k = c.WinDLL("kernel32", use_last_error=True)
k.GetCurrentThreadId.restype = w.DWORD

u.WaitForInputIdle.argtypes = [w.HANDLE, w.DWORD]


def send(hwnd, message, wp=0, lp=0):
    result = c.c_size_t()
    if not u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 5000, c.byref(result)):
        raise RuntimeError(f"message {message:#x} timed out / failed {c.get_last_error()}")
    return result.value


def windows(pid, desktop=None):
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

    if desktop:
        if not u.EnumDesktopWindows(desktop, visit, 0):
            raise c.WinError(c.get_last_error())
    else:
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


class PrivateHost:
    """Bind the child to a verified isolated desktop; never activate it."""

    def __init__(self, target, directory, environment, desktop_name=None):
        import win32api
        import win32con
        import win32event
        import win32process

        self.api, self.event, self.process_api = win32api, win32event, win32process
        inherited = u.GetThreadDesktop(k.GetCurrentThreadId())
        name, needed = c.create_unicode_buffer(256), w.DWORD()
        if not inherited or not u.GetUserObjectInformationW(inherited, 2, name, c.sizeof(name), c.byref(needed)):
            raise c.WinError(c.get_last_error())
        self.name = desktop_name or name.value
        if not self.name.startswith(("CodexSandboxDesktop-", "MMS_REPAIR_")):
            raise RuntimeError(f"properties smoke requires an isolated desktop, refusing {self.name!r}")
        # Enumerate only the selected existing desktop. Newly created desktops
        # can fail USER32 process initialization under the local tool sandbox.
        self.desktop = u.OpenDesktopW(self.name, 0, False, 0x0041)
        if not self.desktop:
            raise c.WinError(c.get_last_error())
        try:
            startup = win32process.STARTUPINFO()
            startup.lpDesktop = "winsta0\\" + self.name
            startup.dwFlags = win32con.STARTF_USESHOWWINDOW
            startup.wShowWindow = win32con.SW_HIDE
            self._handle, self.thread, self.pid, _ = win32process.CreateProcess(
                str(target), subprocess.list2cmdline([str(target)]), None, None, False,
                win32con.CREATE_NO_WINDOW, environment, str(directory), startup)
        except Exception:
            u.CloseDesktop(self.desktop)
            raise
        self.returncode = None

    def poll(self):
        if self.event.WaitForSingleObject(self._handle, 0) == 0:
            self.returncode = self.process_api.GetExitCodeProcess(self._handle)
        return self.returncode

    def wait(self, timeout):
        if self.event.WaitForSingleObject(self._handle, int(timeout * 1000)) != 0:
            raise subprocess.TimeoutExpired("isolated MMS properties host", timeout)
        self.returncode = self.process_api.GetExitCodeProcess(self._handle)
        return self.returncode

    def terminate(self):
        self.api.TerminateProcess(self._handle, 1)

    def close(self):
        self.thread.Close()
        self._handle.Close()
        if not u.CloseDesktop(self.desktop):
            raise c.WinError(c.get_last_error())


def relative_rect(parent, child):
    rect, origin = w.RECT(), w.POINT()
    assert u.GetWindowRect(child, c.byref(rect)) and u.ClientToScreen(parent, c.byref(origin))
    return (rect.left - origin.x, rect.top - origin.y, rect.right - origin.x, rect.bottom - origin.y)


def client_rect(window):
    rect = w.RECT()
    assert u.GetClientRect(window, c.byref(rect))
    return rect


def text(window):
    value = c.create_unicode_buffer(u.GetWindowTextLengthW(window) + 1)
    u.GetWindowTextW(window, value, len(value))
    return value.value


def point(x, y):
    return ((y & 0xffff) << 16) | (x & 0xffff)


def properties_checks(main, directory):
    mounts = {}
    classes = {"toolbar": "MMS.PluginAbi.Toolbar.V1", "workspace": "MMS.PluginAbi.Workspace.V1",
               "properties": "MMS.PluginAbi.Properties.V1"}
    for name, cls in classes.items():
        found = [h for h, kind in children(main) if kind == cls and u.GetParent(h) == main]
        assert len(found) == 1, (name, found)
        mounts[name] = found[0]
    properties = mounts["properties"]
    buttons = sorted((h for h, cls in children(properties) if cls == "Button" and
                      0xa000 <= u.GetDlgCtrlID(h) < 0xc000), key=u.GetDlgCtrlID)
    assert len(buttons) == 4 and all(u.GetParent(h) == properties for h in buttons)
    assert [text(h) for h in buttons] == [f"Property command {i}" for i in range(1, 5)]
    log_path = directory / "probe.log"
    count = log_path.read_text().count("COMMAND_INVOKED")
    for button in buttons:
        send(button, 0x00F5)  # BM_CLICK: native Button -> BN_CLICKED -> registered command
        count += 1
        assert log_path.read_text().count("COMMAND_INVOKED") == count, "property button must dispatch exactly once"
    inputs = sorted(h for h, cls in children(main) if cls.lower() == "edit" and u.GetDlgCtrlID(h) == 0x7101)
    assert len(inputs) == 2
    values = {edit: f"布局保留值 {index + 1} / keep {index + 1}" for index, edit in enumerate(inputs)}
    for edit, value in values.items():
        buffer = c.create_unicode_buffer(value)
        assert send(edit, 0x000C, 0, c.addressof(buffer)), "write native panel text via WM_SETTEXT"
        assert text(edit) == value

    def intersects(a, b):
        return max(a[0], b[0]) < min(a[2], b[2]) and max(a[1], b[1]) < min(a[3], b[3])

    def check(sidebar=None):
        client = client_rect(main)
        boxes = {name: relative_rect(main, hwnd) for name, hwnd in mounts.items()}
        assert len({box[0] for box in boxes.values()}) == 1, boxes
        for name, box in boxes.items():
            assert u.IsWindow(mounts[name]) and box[2] == client.right and box[3] > box[1], (name, box)
            if sidebar is not None:
                assert box[0] == sidebar + 9, (name, box, sidebar)
        assert boxes["toolbar"][1] == 0 and boxes["toolbar"][3] == boxes["workspace"][1], boxes
        assert boxes["workspace"][3] <= boxes["properties"][1], boxes
        assert boxes["properties"][3] == client.bottom - 160, boxes
        for index, a in enumerate(boxes.values()):
            for b in list(boxes.values())[index + 1:]:
                assert not intersects(a, b), "native plugin mounts overlap"
        extent = client_rect(properties)
        for button in buttons:
            assert u.IsWindow(button) and u.GetParent(button) == properties, "layout recreated or detached a property button"
            box = relative_rect(properties, button)
            assert 0 <= box[0] < box[2] <= extent.right and 0 <= box[1] < box[3] <= extent.bottom, box
        # Metrics and mms.scene expose no editor viewport/presentation bounds.
        # Check the actual native viewport-strip HWNDs, without claiming this
        # also verifies D3D scene/HUD pixels or coordinates inside the renderer.
        viewport_controls = 0
        for control_id in [*range(536, 558), *range(570, 578)]:
            control = u.GetDlgItem(main, control_id)
            if not control or not u.GetWindowLongPtrW(control, -16) & 0x10000000:
                continue
            box = relative_rect(main, control)
            if not intersects(box, (0, 0, client.right, client.bottom)):
                continue
            assert not intersects(box, boxes["properties"]), ("native viewport strip overlaps property mount", control_id, box, boxes)
            viewport_controls += 1
        assert viewport_controls, "native viewport strip was not checked"
        assert sorted(h for h, cls in children(main) if cls.lower() == "edit" and u.GetDlgCtrlID(h) == 0x7101) == inputs
        for edit, value in values.items():
            assert u.IsWindow(edit) and text(edit) == value, "layout lost a live panel input or its value"
            panel = u.GetParent(edit)
            box, extent = relative_rect(panel, edit), client_rect(panel)
            assert box == (8, 8, extent.right - 8, 32), (box, extent.right, extent.bottom)
        return boxes

    heights, rows = [], []
    for width, height in ((1440, 900), (960, 820), (1440, 900)):
        assert u.SetWindowPos(main, None, 0, 0, width, height, 0x16)
        boxes = check()
        heights.append(boxes["properties"][3] - boxes["properties"][1])
        rows.append(len({relative_rect(properties, button)[1] for button in buttons}))
    assert heights[1] > heights[0] == heights[2] and rows[1] > rows[0] == rows[2], (heights, rows)

    boxes = check()
    sidebar = boxes["toolbar"][0] - 9
    send(main, 0x0201, 1, point(sidebar + 3, 100))
    for sidebar in (400, 600, 300):
        send(main, 0x0200, 1, point(sidebar, 100))
        check(sidebar)  # Before release, with all three production mounts alive.
    send(main, 0x0202, 0, point(sidebar, 100))
    check(sidebar)

    client = client_rect(main)
    # Fresh isolated Data has no extra width. The first bottom panel ends at
    # x=215; its neighbour's real model selector (436) shifts with this edge.
    selector = u.GetDlgItem(main, 436)
    assert selector
    selector_before = relative_rect(main, selector)
    boxes = check()
    send(main, 0x0201, 1, point(215, client.bottom - 80))
    for edge in (239, 271, 231):
        send(main, 0x0200, 1, point(edge, client.bottom - 80))
        selector_now = relative_rect(main, selector)
        assert selector_now[0] == selector_before[0] + edge - 215, (selector_before, selector_now, edge)
        assert check() == boxes, "bottom-only resizing displaced plugin mounts or changed their reserved width"
    send(main, 0x0202, 0, point(edge, client.bottom - 80))
    assert check() == boxes
    send(buttons[0], 0x00F5)
    assert log_path.read_text().count("COMMAND_INVOKED") == count + 1, "property command lost its identity after layout changes"
    assert log_path.read_text().count("SURFACE_CREATE") == 2, "layout remounted an existing custom panel"
    print("PASS property commands, wrap, three mount bounds, live sidebar/bottom layout and retained panel values", flush=True)
    print("BOUNDARY viewport: native strip HWNDs checked; scene/presentation/HUD pixel bounds unavailable from public metrics/scene", flush=True)
    return [*mounts.values(), *buttons, *inputs]


def run(executable, fixture, directory, filename, mode="gui", desktop_name=None):
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
    properties_mode = mode == "gui-properties-panel"
    process = PrivateHost(target, directory, environment, desktop_name) if properties_mode else subprocess.Popen(
        [str(target)], cwd=directory, env=environment, startupinfo=startup)
    desktop = process.desktop if properties_mode else None
    cleanup_windows = []
    main = None
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"host exited early {process.returncode}")
            main = next((h for h, cls in windows(process.pid, desktop) if cls == "Polygon Movie Maker"), None)
            if main and (directory / "probe.log").exists() and "GUI_REGISTERED" in (directory / "probe.log").read_text():
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"initialization timeout; process windows={windows(process.pid, desktop)}")
        assert not u.IsWindowVisible(main), "host was shown"
        if properties_mode:
            ui_thread = u.GetWindowThreadProcessId(main, None)
            owner_desktop = u.GetThreadDesktop(ui_thread)
            name, needed = c.create_unicode_buffer(256), w.DWORD()
            assert owner_desktop and u.GetUserObjectInformationW(owner_desktop, 2, name, c.sizeof(name), c.byref(needed))
            assert name.value == process.name, ("host did not enter its private desktop", name.value, process.name)
            print(f"PASS isolated production host desktop={name.value}", flush=True)
        idle = u.WaitForInputIdle(w.HANDLE(int(process._handle)), 30000)
        if idle:
            raise RuntimeError(f"host not idle after startup: {idle}; windows={windows(process.pid, desktop)}")
        # Wait until queued registry publication has mounted both controls.
        deadline = time.monotonic() + 5
        toolbar = None
        while time.monotonic() < deadline:
            send(main, 0x116, 0, 0)  # WM_INITMENU, production refresh path
            toolbar = next((h for h, cls in children(main) if cls == "MMS.PluginAbi.Toolbar.V1"), None)
            # The host also owns an overflow button. Exercise the registered
            # plugin control, not whichever Button HWND was created first.
            buttons = [h for h, cls in children(toolbar)
                       if cls == "Button" and 0xa000 <= u.GetDlgCtrlID(h) < 0xc000] if toolbar else []
            if buttons:
                break
            time.sleep(0.05)
        assert toolbar and len(buttons) == 1, "expected exactly one registered probe toolbar button"
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

        if properties_mode:
            cleanup_windows = properties_checks(main, directory)

        send(main, 0x10)  # WM_CLOSE, no document was modified
        process.wait(timeout=15)
        assert process.returncode == 0, process.returncode
        log = (directory / "probe.log").read_text()
        assert "UI_CLOSING" in log and log.splitlines().count("STOP_OK") == 1 and log.splitlines().count("DESTROY") == 1, log
        if "panel" in mode:
            assert log.count("SURFACE_DESTROY") == 2 and log.index("SURFACE_DESTROY") < log.index("STOP_OK"), log
        if properties_mode:
            assert log.splitlines().count("PROPERTIES_REGISTERED") == 1 and log.splitlines().count("PROPERTIES_UNREGISTERED") == 1, log
            assert log.index("PROPERTIES_UNREGISTERED") < log.index("STOP_OK"), log
            assert all(not u.IsWindow(hwnd) for hwnd in [main, *cleanup_windows]), "host cleanup left a native plugin window alive"
            assert not windows(process.pid, desktop), "closed host still owns a desktop window"
        metrics = json.loads((directory / "ui-metrics.jsonl").read_text().splitlines()[-1])
        assert metrics["layout"] > 0 and all(isinstance(v, int) and v >= 0 for v in metrics.values()), metrics
        print(f"PASS {filename}: hidden production startup, menu, toolbar, close; toolbar height={rect.bottom - rect.top}", flush=True)
    except Exception:
        print(f"FAILED {filename}; windows={windows(process.pid, desktop)}", flush=True)
        if (directory / "probe.log").exists():
            print((directory / "probe.log").read_text(), flush=True)
        raise
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        if properties_mode:
            process.close()


if __name__ == "__main__":
    exe, dll, output = map(lambda x: Path(x).resolve(), sys.argv[1:4])
    mode = sys.argv[4] if len(sys.argv) > 4 else "gui"
    desktop_name = None
    if len(sys.argv) > 5:
        if mode != "gui-properties-panel" or len(sys.argv) != 7 or sys.argv[5] != "--desktop":
            raise SystemExit("optional syntax: gui-properties-panel --desktop ISOLATED_DESKTOP_NAME")
        desktop_name = sys.argv[6]
    for name, filename in [("normal", "MikuMikuDance.exe"), ("renamed", "舞台编辑器.exe")]:
        run(exe, dll, output / name, filename, mode, desktop_name)
