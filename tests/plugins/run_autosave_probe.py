"""Run the existing hidden autosave regression with an original PMD fixture."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pmd_selection_fixtures import make_fixture

binary, output = [Path(arg).resolve() for arg in sys.argv[1:]]
output.mkdir(exist_ok=False)
(output / "Data").mkdir()
make_fixture(output / "fixture", 255, [f"toon{i:02}.bmp" for i in range(1, 11)])
shutil.copy2(binary, output / "autosave.exe")
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
env = os.environ.copy()
env["LOCALAPPDATA"] = str(output / "profile")
with (output / "result.log").open("w", encoding="utf-8") as log:
    result = subprocess.run([str(output / "autosave.exe"), str(output)], cwd=output, env=env,
                            startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=120)
report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "exitCode": result.returncode}
(output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
print((output / "result.log").read_text(encoding="utf-8", errors="replace"))
sys.exit(result.returncode)
