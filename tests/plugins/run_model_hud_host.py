"""Run the complete isolated edit host with genuinely movable PMX/PMD bones."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_geometry_probe import accessory, pmx, pmd, text


def movable_model(kind):
    # Change only our generated fixture, before the real importer sees it.
    # Existing geometry fixtures deliberately have no translation control.
    if kind == "pmx":
        data = pmx()
        prefix = text("骨骼0") + text("bone0")
        before = prefix + struct.pack("<3fbiH3f", 0, 0, 0, -1, 0, 26, 0, 1, 0)
        after = prefix + struct.pack("<3fbiH3f", 0, 0, 0, -1, 0, 30, 0, 1, 0)
    else:
        data = pmd()
        prefix = b"root".ljust(20, b"\0")
        before = prefix + struct.pack("<HHBH3f", 65535, 0, 0, 0, 0, 0, 0)
        after = prefix + struct.pack("<HHBH3f", 65535, 0, 1, 0, 0, 0, 0)
    if data.count(before) != 1:
        raise ValueError("generated fixture bone layout changed")
    return data.replace(before, after)


def main():
    binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "pluginSha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "cases": []}
    for kind, filename in (("pmx", "MikuMikuDance.exe"), ("pmd", "自定义名称.exe")):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        (root / "data").mkdir()
        model = root / ("model." + kind)
        model.write_bytes(movable_model(kind))
        item = root / "triangle.x"
        item.write_text(accessory(), encoding="ascii")
        package = root / "PLUGINS" / "编辑" / "测试"
        package.mkdir(parents=True)
        shutil.copy2(plugin, package / "manager_edit_probe.dll")
        manifest = dict(schemaVersion=1, id="org.example.editprobe", name="Model HUD history probe", version="1.0.0",
                        kind="native", abi=dict(major=1, minMinor=0), entry="manager_edit_probe.dll", architecture="x64",
                        minWindows="6.1.7601", capabilities=["ui.command", "ui.element"],
                        requiresServices=[dict(id="mms.ui", major=1, minMinor=0)], dependencies=[],
                        nativeDependencies=[], reloadPolicy="restart")
        (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
        executable = root / filename
        shutil.copy2(binary, executable)
        env = os.environ.copy()
        env.update(LOCALAPPDATA=str(root / "local"), MMS_SCENE_TEST_MODEL=str(model),
                   MMS_SCENE_TEST_ACCESSORY=str(item), MMS_ABI_PROBE_LOG=str(root / "probe.log"),
                   MMS_UI_METRICS=str(root / "ui-metrics.jsonl"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(executable), "--edit-history-only"], cwd=root, env=env,
                                    startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=150)
        report["cases"].append(dict(kind=kind, name=filename, modelSha256=hashlib.sha256(model.read_bytes()).hexdigest(),
                                    exitCode=result.returncode))
        (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        print(kind, "exit", result.returncode, flush=True)
        if result.returncode:
            print((root / "result.log").read_text(encoding="utf-8", errors="replace")[-6500:], flush=True)
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
