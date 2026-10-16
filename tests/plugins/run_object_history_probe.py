"""Verify retained original PMX/PMD/X fixtures in isolated hidden hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_geometry_probe import accessory, pmd, pmx

binary, output = [Path(value).resolve() for value in sys.argv[1:]]
output.mkdir(exist_ok=False)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pmd_selection_fixtures import make_fixture

report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "cases": []}
for kind, filename in (("pmx", "MikuMikuDance.exe"), ("pmd", "自定义名称.exe")):
    root = output / kind
    make_fixture(root, 255, [""] * 10)
    (root / "model.pmx").write_bytes(pmx())
    (root / "model.pmd").write_bytes(pmd())
    (root / "triangle.x").write_text(accessory(), encoding="ascii")
    (root / "Data").mkdir()
    (root / "local").mkdir()
    shutil.copy2(binary, root / filename)
    model = root / ("model." + kind)
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root / "local"), MMS_SCENE_TEST_MODEL=str(model),
               MMS_SCENE_TEST_ACCESSORY=str(root / "triangle.x"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    with (root / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(root / filename), "--object-history-only"], cwd=root, env=env,
                                startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace"))
    report["cases"].append({"kind": kind, "name": filename, "exitCode": result.returncode})
(output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
sys.exit(any(case["exitCode"] for case in report["cases"]))
