"""Run frozen morph sampling against a real PMX in an isolated hidden host."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, model, output = [Path(value).resolve() for value in sys.argv[1:4]]
mode = sys.argv[4] if len(sys.argv) == 5 else '--morph-sampling-only'
if len(sys.argv) not in (4, 5) or mode not in ('--morph-sampling-only', '--display-sampling-only'):
    raise ValueError('expected binary model output [--display-sampling-only]')
output.mkdir(exist_ok=False)
for name in ('data', 'temp', 'local', 'PLUGINS'):
    (output / name).mkdir()
shutil.copy2(binary, output / 'probe.exe')
shutil.copy2(model, output / 'model.pmx')
env = dict(os.environ, TEMP=str(output/'temp'), TMP=str(output/'temp'),
    LOCALAPPDATA=str(output/'local'), MMS_PLUGIN_TEST_DIR=str(output/'PLUGINS'),
    MMS_SCENE_TEST_MODEL=str(output/'model.pmx'))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
command = [str(output/'probe.exe'), mode]
with (output/'result.log').open('w', encoding='utf-8') as log:
    result = subprocess.run(command, cwd=output, env=env, startupinfo=startup,
        creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=180)
report = dict(command=command, exitCode=result.returncode,
    inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, model)})
(output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print((output/'result.log').read_text(encoding='utf-8', errors='replace')[-6000:])
sys.exit(result.returncode)
