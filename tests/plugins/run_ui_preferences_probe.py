"""Check layout persistence in isolated hidden production hosts, including EXE rename."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from host_smoke import u, w, children, send, windows


def main():
    executable, fixture, root = (Path(value).resolve() for value in sys.argv[1:4])
    on_close = len(sys.argv) > 4 and sys.argv[4] == "on-close"
    root.mkdir(parents=True, exist_ok=False)
    (root / "Data").mkdir()
    package = root / "PLUGINS" / "界面" / "布局"
    package.mkdir(parents=True)
    shutil.copy2(fixture, package / "lifecycle.dll")
    manifest = package / "plugin.json"
    manifest.write_text(json.dumps(dict(
        schemaVersion=1, id="org.example.gui-layout", name="Layout persistence probe", version="1.0.0", kind="native",
        abi=dict(major=1, minMinor=0), entry="lifecycle.dll", architecture="x64", minWindows="6.1.7601",
        capabilities=["ui.command", "ui.element"], requiresServices=[dict(id="mms.ui", major=1, minMinor=0)],
        dependencies=[], nativeDependencies=[], reloadPolicy="restart")), encoding="utf-8")
    preferences = root / "userdata" / "MMS" / "plugins" / "ui-layout.json"
    log = root / "probe.log"
    environment = os.environ.copy()
    environment.update(LOCALAPPDATA=str(root / "userdata"), MMS_ABI_PROBE_LOG=str(log))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    results = []
    for phase, filename in [("edit", "MikuMikuDance.exe"), ("missing-plugin", "MikuMikuDance.exe"), ("restore", "舞台布局.exe")]:
        if log.exists():
            log.unlink()
        if phase == "missing-plugin":
            manifest.rename(package / "plugin.off")
        if phase == "restore":
            (package / "plugin.off").rename(manifest)
        environment.pop("MMS_UI_LAYOUT_ON_CLOSE", None)
        if phase == "edit" and on_close:
            environment["MMS_UI_LAYOUT_ON_CLOSE"] = "1"
        target = root / filename
        if not target.exists():
            shutil.copy2(executable, target)
        process = subprocess.Popen([str(target)], cwd=root, env=environment, startupinfo=startup)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                assert process.poll() is None, f"early exit {process.returncode}"
                window = next((h for h, name in windows(process.pid) if name == "Polygon Movie Maker"), None)
                if window and (phase == "missing-plugin" or log.exists() and "GUI_REGISTERED" in log.read_text()):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"startup timeout: {windows(process.pid)}")
            assert not u.IsWindowVisible(window), "isolated host unexpectedly visible"
            assert u.WaitForInputIdle(w.HANDLE(int(process._handle)), 30000) == 0
            send(window, 0x116)
            toolbar = next(h for h, name in children(window) if name == "MMS.PluginAbi.Toolbar.V1")
            buttons = [h for h, name in children(toolbar) if name == "Button"]
            if phase == "edit":
                assert len(buttons) == 1, buttons
            if phase == "edit" and not on_close:
                menu = u.GetMenu(window)
                popup = u.GetSubMenu(menu, u.GetMenuItemCount(menu) - 1)
                send(window, 0x111, u.GetMenuItemID(popup, 0))
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    send(window, 0x116)
                    if "LAYOUT_HIDDEN" in log.read_text():
                        break
                    time.sleep(0.05)
                send(window, 0x116)
                assert "LAYOUT_HIDDEN" in log.read_text(), log.read_text()
                assert not [h for h, name in children(toolbar) if name == "Button"], "committed hide not reflected"
            elif phase != "edit":
                assert not buttons, "saved hidden button was recreated"
                assert preferences.read_bytes() == saved, "restart changed saved user overrides"
            send(window, 0x10)
            process.wait(timeout=15)
            assert process.returncode == 0, process.returncode
            if phase == "edit":
                assert "LAYOUT_HIDDEN" in log.read_text(), log.read_text()
                saved = preferences.read_bytes()
                document = json.loads(saved)
                assert document["schemaVersion"] == 1
                assert any(e["id"] == "org.example.gui-layout.button" and e["visible"] is False for e in document["elements"])
            else:
                assert preferences.read_bytes() == saved, "shutdown removed dormant preferences"
            results.append(dict(phase=phase, filename=filename, exitCode=process.returncode))
            print(f"PASS {phase}: hidden host, saved layout, clean shutdown", flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
    (root / "report.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
