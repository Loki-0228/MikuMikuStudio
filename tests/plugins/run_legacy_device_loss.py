"""Inject loss/status failures around a real D3D9 device with the original plugins loaded.

This does not change display mode or claim a physical GPU removal was tested.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--require-on12', action='store_true')
args = parser.parse_args()
build, fixture = args.build.resolve(), args.fixture.resolve()
assert hashlib.sha256((fixture / 'PLUGINS/LEGACY/test.camera/MMDFirstPersonCamera.dll').read_bytes()).hexdigest() == 'd3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef'
root = Path(tempfile.mkdtemp(prefix='legacy-loss-', dir=build.parent))
results = []
for entry in ['frame', 'readback', 'record-frame']:
    for scenario in ['lost-ready', 'lost-reset', 'reset-fail', 'fatal']:
        directory = root / str(len(results))
        shutil.copytree(fixture, directory)
        name = ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe'][len(results) % 3]
        exe = directory / name
        shutil.copy2(build / 'tests/device-lost/Release/device_lost_wait_tests.exe', exe)
        env = os.environ.copy()
        env.update(TEMP=str(directory/'temp'), TMP=str(directory/'temp'), LOCALAPPDATA=str(directory/'local'),
            MIKUMIKUSTUDIO_TRACE_FILE=str(directory/'loss-runtime.log'), MMS_DEVICE_TEST_MODEL=str(directory/'中文特效/block_simple.efk'))
        with (directory/'loss.log').open('wb') as output:
            run = subprocess.run([str(exe), '--entry', entry, '--case', scenario, '--mode', 'windowed'],
                cwd=directory, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        log = (directory/'loss.log').read_text(encoding='utf-8', errors='replace')
        trace = (directory/'loss-runtime.log').read_text(encoding='utf-8', errors='replace')
        line = next((line[7:] for line in log.splitlines() if line.startswith('RESULT ')), None)
        all_originals = 'started (first_person_camera_v4)' in trace and 'started (effekseer_v2)' in trace and 'LEGACY_MME org.mikumikustudio.legacy.mme loaded from' in trace
        on12 = 'MME_BACKEND D3D12 active via D3D9On12' in trace
        case = dict(actualOn12=on12, entry=entry, scenario=scenario, exe=name, directory=str(directory),
            originalPluginsStarted=all_originals, exitCode=run.returncode, measurements=json.loads(line) if line else None,
            passed=run.returncode == 0 and all_originals and line is not None and (not args.require_on12 or on12))
        results.append(case)
        (root/'summary.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        print(json.dumps(case, ensure_ascii=True), flush=True)
        if not case['passed']: print(root/'summary.json'); raise SystemExit(1)
print(root/'summary.json')
