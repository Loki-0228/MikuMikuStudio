"""Exercise the shipped DLL in an isolated copy of an unchanged MMS executable."""
import argparse
import base64
import ctypes as C
from ctypes import wintypes as W
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time


u = C.WinDLL("user32", use_last_error=True)
k = C.WinDLL("kernel32", use_last_error=True)
p = C.WinDLL("psapi", use_last_error=True)
# Match screen coordinates to PrintWindow pixels on scaled Windows desktops.
u.SetProcessDPIAware()
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
u.EnumWindows.argtypes = [CALLBACK, W.LPARAM]
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
u.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
u.GetMenu.argtypes = [W.HWND]
u.GetMenu.restype = W.HMENU
u.GetMenuItemCount.argtypes = [W.HMENU]
u.GetSubMenu.argtypes = [W.HMENU, C.c_int]
u.GetSubMenu.restype = W.HMENU
u.GetMenuItemID.argtypes = [W.HMENU, C.c_int]
u.GetMenuStringW.argtypes = [W.HMENU, W.UINT, W.LPWSTR, C.c_int, W.UINT]
u.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SendMessageW.restype = C.c_ssize_t
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.GetDlgItem.argtypes = [W.HWND, C.c_int]
u.GetDlgItem.restype = W.HWND
u.IsWindowEnabled.argtypes = [W.HWND]
u.IsWindowVisible.argtypes = [W.HWND]
u.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.ClientToScreen.argtypes = [W.HWND, C.POINTER(W.POINT)]
u.GetMenuItemRect.argtypes = [W.HWND, W.HMENU, W.UINT, C.POINTER(W.RECT)]
k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.OpenProcess.restype = W.HANDLE
k.CloseHandle.argtypes = [W.HANDLE]
k.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
k.CreateToolhelp32Snapshot.restype = W.HANDLE
p.PROCESSENTRY32W = type("PROCESSENTRY32W", (C.Structure,), {"_fields_": [
    ("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ProcessID", W.DWORD),
    ("th32DefaultHeapID", C.c_size_t), ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
    ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", W.LONG), ("dwFlags", W.DWORD),
    ("szExeFile", W.WCHAR * 260)]})
k.Process32FirstW.argtypes = [W.HANDLE, C.POINTER(p.PROCESSENTRY32W)]
k.Process32NextW.argtypes = [W.HANDLE, C.POINTER(p.PROCESSENTRY32W)]
k.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
k.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
k.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
p.EnumProcessModules.argtypes = [W.HANDLE, C.POINTER(W.HMODULE), W.DWORD, C.POINTER(W.DWORD)]
p.GetModuleFileNameExW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]


def windows(pid):
    found = []
    @CALLBACK
    def visit(window, _):
        process = W.DWORD()
        u.GetWindowThreadProcessId(window, C.byref(process))
        if process.value == pid:
            name, title = C.create_unicode_buffer(256), C.create_unicode_buffer(512)
            u.GetClassNameW(window, name, len(name))
            u.GetWindowTextW(window, title, len(title))
            found.append((window, name.value, title.value))
        return True
    u.EnumWindows(visit, 0)
    return found


def until(fn, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.1)
    raise RuntimeError("Timed out waiting for UI state")


def modules(pid):
    handle = k.OpenProcess(0x410, False, pid)
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        entries = (W.HMODULE * 2048)()
        needed = W.DWORD()
        if not p.EnumProcessModules(handle, entries, C.sizeof(entries), C.byref(needed)):
            raise C.WinError(C.get_last_error())
        paths = []
        for module in entries[:needed.value // C.sizeof(W.HMODULE)]:
            buf = C.create_unicode_buffer(32768)
            p.GetModuleFileNameExW(handle, module, buf, len(buf))
            paths.append(buf.value)
        return paths
    finally:
        k.CloseHandle(handle)


def child_process(parent):
    snapshot = k.CreateToolhelp32Snapshot(2, 0)
    if snapshot == W.HANDLE(-1).value:
        return None
    try:
        entry = p.PROCESSENTRY32W()
        entry.dwSize = C.sizeof(entry)
        if k.Process32FirstW(snapshot, C.byref(entry)):
            while True:
                if entry.th32ParentProcessID == parent:
                    return entry.th32ProcessID
                if not k.Process32NextW(snapshot, C.byref(entry)):
                    break
        return None
    finally:
        k.CloseHandle(snapshot)


def wait_process(pid, seconds):
    process = k.OpenProcess(0x100000 | 0x1000, False, pid)
    if not process:
        return None
    try:
        if k.WaitForSingleObject(process, int(seconds * 1000)) != 0:
            return None
        result = W.DWORD()
        return result.value if k.GetExitCodeProcess(process, C.byref(result)) else None
    finally:
        k.CloseHandle(process)


def text(window):
    count = u.SendMessageW(window, 14, 0, 0)
    buf = C.create_unicode_buffer(count + 1)
    u.SendMessageW(window, 13, len(buf), C.addressof(buf))
    return buf.value


def menu_item(main):
    menu = u.GetMenu(main)
    for i in range(u.GetMenuItemCount(menu)):
        label = C.create_unicode_buffer(256)
        u.GetMenuStringW(menu, i, label, len(label), 0x400)
        if label.value in ("数值修复", "Value repair", "数値修復"):
            popup = u.GetSubMenu(menu, i)
            item = C.create_unicode_buffer(256)
            u.GetMenuStringW(popup, 0, item, len(item), 0x400)
            rect = W.RECT()
            assert u.GetMenuItemRect(main, menu, i, C.byref(rect))
            return {"command": u.GetMenuItemID(popup, 0), "title": label.value,
                    "item": item.value, "rect": [rect.left, rect.top, rect.right, rect.bottom]}
    return None


def verify_launcher(window):
    expected = {300: "MMS executable", 302: "Browse...", 303: "Start MMS", 2: "Close"}
    assert text(window) == "MMS Curve Repair Launcher", text(window)
    rects = {}
    client = W.RECT()
    assert u.GetClientRect(window, C.byref(client))
    origin = W.POINT()
    assert u.ClientToScreen(window, C.byref(origin))
    for control_id in (300, 301, 302, 303, 304, 2):
        control = u.GetDlgItem(window, control_id)
        if control_id in expected:
            assert text(control) == expected[control_id], (control_id, text(control))
        rect = W.RECT()
        assert u.GetWindowRect(control, C.byref(rect))
        box = [rect.left - origin.x, rect.top - origin.y, rect.right - origin.x, rect.bottom - origin.y]
        assert 0 <= box[0] < box[2] <= client.right, (control_id, box, client.right)
        assert 0 <= box[1] < box[3] <= client.bottom, (control_id, box, client.bottom)
        rects[control_id] = box
    assert rects[303][2] < rects[2][0], rects
    assert rects[304][3] < rects[303][1], rects
    assert text(u.GetDlgItem(window, 304)) == "The launcher loads the plug-in into MMS before starting it. The MMS executable stays intact."
    return {"language": "English", "client": [client.right, client.bottom], "controls": rects}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sidecar_document(data):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, ("duplicate sidecar field", key)
            result[key] = value
        return result

    def invalid_constant(value):
        raise AssertionError(("non-JSON sidecar number", value))

    document = json.loads(data, parse_float=Decimal, parse_int=Decimal,
                          object_pairs_hook=unique_object, parse_constant=invalid_constant)
    assert isinstance(document, dict), "sidecar root must be an object"
    assert type(document["schemaVersion"]) is Decimal and document["schemaVersion"] == 1
    for field, length in (("projectId", 32), ("saveId", 32), ("mainSha256", 64)):
        assert isinstance(document[field], str) and re.fullmatch(r"[0-9a-fA-F]{%d}" % length, document[field]), field
    assert isinstance(document["plugins"], dict), "sidecar plugins must be an object"
    return document


def same_json(left, right):
    # Decimal preserves unknown long integers and decimal tokens exactly.
    # Check scalar types too: JSON true must not compare equal to number 1.
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(same_json(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(same_json(a, b) for a, b in zip(left, right))
    return left == right


def verify_repaired_sidecar(fixture, repaired, original_bytes, original_document, report):
    source = Path(str(fixture) + ".mms-plugins.json")
    destination = Path(str(repaired) + ".mms-plugins.json")
    if original_bytes is None:
        assert not source.exists(), "repair unexpectedly created an original sidecar"
        assert not destination.exists(), "repair created a sidecar for a project without one"
        report["sidecar"].update(copy_absent=True, original_absent=True)
        return
    assert source.read_bytes() == original_bytes, "repair changed the original sidecar bytes"
    assert destination.is_file(), "repair omitted the existing sidecar"
    document = sidecar_document(destination.read_bytes())
    assert document["mainSha256"].lower() == sha(repaired), "repaired PMM and sidecar hashes differ"
    assert document["saveId"].lower() != original_document["saveId"].lower(), "repair reused the original save identity"
    assert document["projectId"] == original_document["projectId"], "repair changed the project identity"
    assert same_json(document["schemaVersion"], original_document["schemaVersion"])
    assert same_json(document["plugins"], original_document["plugins"]), "repair changed plugin records or unknown nested fields"
    unchanged = lambda value: {key: item for key, item in value.items() if key not in ("mainSha256", "saveId")}
    assert same_json(unchanged(document), unchanged(original_document)), "repair changed unknown root fields"
    report["sidecar"].update(copy=str(destination), copy_sha256=sha(destination),
                             paired_main_sha256=document["mainSha256"], save_id=document["saveId"],
                             original_preserved=True, project_id_preserved=True,
                             schema_preserved=True, plugin_records_preserved=True,
                             unknown_fields_preserved=True, save_id_changed=True)


def capture_window(window, path):
    # Print only this plugin window, including when another app is foreground.
    from PIL import Image
    g = C.WinDLL("gdi32", use_last_error=True)
    u.GetWindowDC.argtypes, u.GetWindowDC.restype = [W.HWND], W.HDC
    u.ReleaseDC.argtypes = [W.HWND, W.HDC]
    u.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
    g.CreateCompatibleDC.argtypes, g.CreateCompatibleDC.restype = [W.HDC], W.HDC
    g.CreateCompatibleBitmap.argtypes, g.CreateCompatibleBitmap.restype = [W.HDC, C.c_int, C.c_int], W.HBITMAP
    g.SelectObject.argtypes, g.SelectObject.restype = [W.HDC, W.HANDLE], W.HANDLE
    g.DeleteObject.argtypes = [W.HANDLE]
    g.DeleteDC.argtypes = [W.HDC]
    g.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, C.c_void_p, C.c_void_p, W.UINT]
    rect = W.RECT()
    u.GetWindowRect(window, C.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    screen = u.GetWindowDC(window)
    dc = g.CreateCompatibleDC(screen)
    bitmap = g.CreateCompatibleBitmap(screen, width, height)
    old = g.SelectObject(dc, bitmap)
    try:
        assert u.PrintWindow(window, dc, 2), "PrintWindow"
        g.SelectObject(dc, old)
        import struct
        header = C.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0))
        pixels = C.create_string_buffer(width * height * 4)
        assert g.GetDIBits(dc, bitmap, 0, height, pixels, header, 0) == height
        Image.frombytes("RGB", (width, height), pixels.raw, "raw", "BGRX").save(path)
    finally:
        g.SelectObject(dc, old)
        g.DeleteObject(bitmap)
        g.DeleteDC(dc)
        u.ReleaseDC(window, screen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", type=Path, required=True)
    ap.add_argument("--launcher", type=Path, required=True)
    ap.add_argument("--dll", type=Path, required=True)
    ap.add_argument("--fixture", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--data", type=Path)
    ap.add_argument("--language", type=int, choices=(0, 1, 2), default=2)
    ap.add_argument("--reopen", action="store_true")
    args = ap.parse_args()
    for name in ("host", "launcher", "dll", "fixture", "output", "data"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    runtime = args.output / "runtime"
    runtime.mkdir(exist_ok=False)
    shutil.copy2(args.host, runtime / args.host.name)
    shutil.copy2(args.launcher, runtime / "MMSCurveRepairLauncher.exe")
    shutil.copy2(args.dll, runtime / "MMSCurveRepair.dll")
    shutil.copytree(args.data or args.host.parent / "Data", runtime / "Data")
    config = runtime / "Data" / "mmconfig.ini"
    lines = config.read_bytes().splitlines()
    lines[8] = str(args.language).encode("ascii")
    config.write_bytes(b"\r\n".join(lines) + b"\r\n")
    fixture_hash = sha(args.fixture)
    report = {"host_sha256": sha(args.host), "launcher_sha256": sha(args.launcher),
              "dll_sha256": sha(args.dll), "fixture_sha256": fixture_hash}
    original_sidecar = Path(str(args.fixture) + ".mms-plugins.json")
    original_sidecar_bytes = original_sidecar.read_bytes() if original_sidecar.exists() else None
    original_sidecar_document = None
    report["sidecar"] = {"original": str(original_sidecar), "present": original_sidecar_bytes is not None}
    if original_sidecar_bytes is not None:
        original_sidecar_document = sidecar_document(original_sidecar_bytes)
        assert original_sidecar_document["mainSha256"].lower() == fixture_hash, "fixture sidecar does not match its PMM"
        report["sidecar"].update(original_sha256=hashlib.sha256(original_sidecar_bytes).hexdigest(),
                                 original_bytes_base64=base64.b64encode(original_sidecar_bytes).decode("ascii"),
                                 original_json=original_sidecar_bytes.decode("utf-8-sig"),
                                 project_id=original_sidecar_document["projectId"],
                                 original_save_id=original_sidecar_document["saveId"],
                                 plugin_count=len(original_sidecar_document["plugins"]))
    launcher = subprocess.Popen([str(runtime / "MMSCurveRepairLauncher.exe")], cwd=runtime)
    host_pid = None
    try:
        launcher_window = until(lambda: next((h for h, cls, _ in windows(launcher.pid)
                                            if cls == "MMS.CurveRepair.Launcher"), None))
        # Exercise the requested host even when its EXE has a custom name.
        # The launcher's default path exists only for MikuMikuDance.exe.
        selected_host = str(runtime / args.host.name)
        host_buffer = C.create_unicode_buffer(selected_host)
        path_control = u.GetDlgItem(launcher_window, 301)
        assert u.SendMessageW(path_control, 12, 0, C.addressof(host_buffer))
        assert text(path_control) == selected_host
        report["selected_host"] = selected_host
        report["launcher_ui"] = verify_launcher(launcher_window)
        capture_window(launcher_window, args.output / "launcher-window.png")
        u.PostMessageW(launcher_window, 0x111, 303, 0)
        host_pid = until(lambda: child_process(launcher.pid), 50)
        main_window = until(lambda: next((h for h, cls, _ in windows(host_pid)
                                        if cls == "Polygon Movie Maker" and u.IsWindowVisible(h)), None), 50)
        until(lambda: str(runtime / "MMSCurveRepair.dll").lower() in [s.lower() for s in modules(host_pid)])
        item = until(lambda: menu_item(main_window))
        assert item["title"] == ("数値修復", "Value repair", "数值修复")[args.language], item
        assert item["item"] == ("保存済みカメラ数値を修復…", "Repair saved camera values…", "修复已保存的镜头数值…")[args.language], item
        report["menu"] = item
        capture_window(main_window, args.output / "host-menu.png")
        command = item["command"]
        report["loaded_plugin"] = str(runtime / "MMSCurveRepair.dll")
        report["menu_command"] = command
        u.PostMessageW(main_window, 0x111, command, 0)
        dialog = until(lambda: next((h for h, cls, _ in windows(host_pid) if cls == "MMS.CurveRepair.1"), None))
        path_buffer = C.create_unicode_buffer(str(args.fixture))
        u.SendMessageW(u.GetDlgItem(dialog, 101), 12, 0, C.addressof(path_buffer))
        u.PostMessageW(dialog, 0x111, 105, 0)
        until(lambda: u.IsWindowEnabled(u.GetDlgItem(dialog, 107)))
        preview = text(u.GetDlgItem(dialog, 106))
        assert "44" in preview and "5.587" in preview, preview
        assert ("カメラキー", "Camera keys", "镜头关键帧")[args.language] in preview
        report["preview"] = preview
        report["language"] = args.language
        capture_window(dialog, args.output / "repair-window.png")
        before = set(args.fixture.parent.glob("*_repaired_*.pmm"))
        u.PostMessageW(dialog, 0x111, 107, 0)
        modal = until(lambda: next((h for h, cls, _ in windows(host_pid) if cls == "#32770"), None))
        new = set(args.fixture.parent.glob("*_repaired_*.pmm")) - before
        assert len(new) == 1, new
        repaired = new.pop()
        assert repaired.with_suffix(".emm").read_bytes() == args.fixture.with_suffix(".emm").read_bytes()
        assert sha(args.fixture) == fixture_hash
        assert (Path(str(repaired) + ".repair.txt")).is_file()
        verify_repaired_sidecar(args.fixture, repaired, original_sidecar_bytes, original_sidecar_document, report)
        report.update(repaired=str(repaired), original_preserved=True, emm_copied=True)
        u.PostMessageW(modal, 0x111, 1, 0)
        u.PostMessageW(dialog, 0x10, 0, 0)
        until(lambda: not any(cls == "MMS.CurveRepair.1" for _, cls, _ in windows(host_pid)))
        u.PostMessageW(main_window, 0x10, 0, 0)
        report["host_exit"] = wait_process(host_pid, 20)
        assert report["host_exit"] == 0, report["host_exit"]
        assert sha(runtime / args.host.name) == report["host_sha256"]
        report["host_unchanged"] = True
        if args.reopen:
            process = subprocess.Popen([str(runtime / args.host.name), str(repaired)], cwd=runtime)
            host_pid = process.pid
            reopened = until(lambda: next((h for h, cls, title in windows(process.pid)
                                          if cls == "Polygon Movie Maker" and u.IsWindowVisible(h)
                                          and "_repaired_" in title), None), 50)
            time.sleep(2)
            assert not any(cls == "#32770" and u.IsWindowVisible(h) for h, cls, _ in windows(process.pid))
            report["reopened_window"] = [title for h, _, title in windows(process.pid) if h == reopened][0]
            u.PostMessageW(reopened, 0x10, 0, 0)
            report["reopened_exit"] = process.wait(timeout=20)
            assert report["reopened_exit"] == 0
            verify_repaired_sidecar(args.fixture, repaired, original_sidecar_bytes, original_sidecar_document, report)
            report["sidecar"]["reopened_with_host"] = True
    except Exception:
        report["windows_on_failure"] = windows(host_pid or launcher.pid)
        raise
    finally:
        if host_pid and wait_process(host_pid, 0) is None:
            target = k.OpenProcess(0x1, False, host_pid)
            if target:
                k.TerminateProcess(target, 1)
                k.WaitForSingleObject(target, 10000)
                k.CloseHandle(target)
        if launcher.poll() is None:
            launcher.terminate()
            launcher.wait(timeout=10)
        (args.output / "runtime-verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
