"""Run the real PMM/scene service probe in a fresh, hidden copy of MMS."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, model, output = [Path(value).resolve() for value in sys.argv[1:]]
output.mkdir(exist_ok=False)
(output / "Data").mkdir()
(output / "local").mkdir()
shutil.copy2(binary, output / "probe.exe")
shutil.copy2(model, output / "model.pmx")
env = os.environ.copy()
env.update(LOCALAPPDATA=str(output / "local"), MMS_SCENE_TEST_MODEL=str(output / "model.pmx"))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
command = [str(output / "probe.exe"), "--scene-only"]
with (output / "result.log").open("w", encoding="utf-8") as log:
    result = subprocess.run(command, cwd=output, env=env, startupinfo=startup,
                            stdout=log, stderr=subprocess.STDOUT, timeout=120)
report = {"command": command, "exitCode": result.returncode,
          "inputs": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, model)}}
(output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print((output / "result.log").read_text(encoding="utf-8", errors="replace"))
sys.exit(result.returncode)
