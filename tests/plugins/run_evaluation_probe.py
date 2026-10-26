"""Run a production host and evaluation plugin in an isolated hidden directory."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, model, plugin, output = [Path(value).resolve() for value in sys.argv[1:5]]
mode = sys.argv[5] if len(sys.argv) == 6 else '--evaluation-only'
if len(sys.argv) not in (5, 6) or mode not in ('--evaluation-only', '--evaluation-morph-only'):
    raise ValueError('expected binary model plugin output [--evaluation-morph-only]')
output.mkdir(exist_ok=False)
for name in ('data', 'temp', 'local'):
    (output / name).mkdir()
package = output / 'PLUGINS' / 'evaluation'
package.mkdir(parents=True)
shutil.copy2(binary, output / 'probe.exe')
shutil.copy2(model, output / 'model.pmx')
shutil.copy2(plugin, package / 'mms_evaluation_host_probe.dll')
manifest = dict(schemaVersion=1, id='org.example.evaluation-host', name='Evaluation test',
    version='1.0.0', kind='native', abi=dict(major=1, minMinor=0),
    entry='mms_evaluation_host_probe.dll', architecture='x64', minWindows='6.1.7601',
    capabilities=['scene.evaluate'], requiresServices=[dict(id='mms.scene.evaluate', major=1,
        minMinor=1 if mode == '--evaluation-morph-only' else 0)],
    dependencies=[], nativeDependencies=[], reloadPolicy='restart')
(package / 'plugin.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
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
    inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, model, plugin)})
(output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print((output/'result.log').read_text(encoding='utf-8', errors='replace')[-6000:])
sys.exit(result.returncode)
