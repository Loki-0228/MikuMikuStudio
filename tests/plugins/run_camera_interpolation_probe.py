"""Real camera interpolation commands and edit DLL in isolated PMX/PMD hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_native_registration_probe import pmx, pmd
from run_geometry_probe import accessory


def main():
    binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"binary": str(binary), "sha256": digest(binary), "pluginSha256": digest(plugin), "cases": []}
    for kind, filename in (("pmx", "MikuMikuDance.exe"), ("pmd", "自定义名称.exe")):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        model = root / ("camera-interpolation." + kind)
        model.write_bytes(pmx() if kind == "pmx" else pmd())
        obj = root / "camera-accessory.x"
        obj.write_text(accessory(), encoding="ascii")
        before = {str(path): digest(path) for path in (model, obj)}
        for name in ("Data", "local", "roaming", "temp"):
            (root / name).mkdir()
        package = root / "PLUGINS" / "编辑" / "相机插值测试"
        package.mkdir(parents=True)
        shutil.copy2(plugin, package / "manager_edit_probe.dll")
        manifest = dict(schemaVersion=1, id="org.example.editprobe", name="Camera interpolation history probe", version="1.0.0",
                        kind="native", abi=dict(major=1, minMinor=0), entry="manager_edit_probe.dll", architecture="x64",
                        minWindows="6.1.7601", capabilities=["ui.command", "ui.element"],
                        requiresServices=[dict(id="mms.ui", major=1, minMinor=0)], dependencies=[],
                        nativeDependencies=[], reloadPolicy="restart")
        (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
        shutil.copy2(binary, root / filename)
        env = os.environ.copy()
        env.update(LOCALAPPDATA=str(root / "local"), APPDATA=str(root / "roaming"), TEMP=str(root / "temp"), TMP=str(root / "temp"),
                   MMS_SCENE_TEST_MODEL=str(model), MMS_SCENE_TEST_ACCESSORY=str(obj))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(root / filename), "--camera-interpolation-only"], cwd=root, env=env,
                                    startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        text = (root / "result.log").read_text(encoding="utf-8", errors="replace")
        print(kind, text)
        unchanged = all(path.is_file() and digest(path) == before[str(path)] for path in (model, obj))
        marker = "PASS actual camera 431/432, six channels/all, real DLL mixed undo/redo, no-op and blocked requests"
        report["cases"].append({"kind": kind, "name": filename, "exitCode": result.returncode,
                                "fixturesUnchanged": unchanged, "completed": marker in text,
                                "modelSha256": before[str(model)], "accessorySha256": before[str(obj)]})
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    sys.exit(any(case["exitCode"] or not case["fixturesUnchanged"] or not case["completed"] for case in report["cases"]))


if __name__ == "__main__":
    main()
