"""Run original PMX/PMD fixtures with a real edit DLL in isolated hidden hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from run_geometry_probe import pmd, pmx

binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
output.mkdir(exist_ok=False)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pmd_selection_fixtures import make_fixture

report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
          "pluginSha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "cases": []}
for kind, filename in (("pmx", "MikuMikuDance.exe"), ("pmd", "自定义名称.exe")):
    root = output / kind
    make_fixture(root, 255, [""] * 10)
    model = root / ("model." + kind)
    model.write_bytes(pmx() if kind == "pmx" else pmd())
    (root / "Data").mkdir()
    (root / "local").mkdir()
    package = root / "PLUGINS" / "编辑" / "测试"
    package.mkdir(parents=True)
    shutil.copy2(plugin, package / "manager_edit_probe.dll")
    manifest = dict(schemaVersion=1, id="org.example.editprobe", name="Edit history probe", version="1.0.0",
                    kind="native", abi=dict(major=1, minMinor=0), entry="manager_edit_probe.dll", architecture="x64",
                    minWindows="6.1.7601", capabilities=["ui.command", "ui.element"],
                    requiresServices=[dict(id="mms.ui", major=1, minMinor=0)], dependencies=[],
                    nativeDependencies=[], reloadPolicy="restart")
    (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
    shutil.copy2(binary, root / filename)
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root / "local"), MMS_SCENE_TEST_MODEL=str(model))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    with (root / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(root / filename), "--edit-history-only"], cwd=root, env=env,
                                startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace"))
    report["cases"].append({"kind": kind, "name": filename, "exitCode": result.returncode})
(output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
sys.exit(any(case["exitCode"] for case in report["cases"]))
