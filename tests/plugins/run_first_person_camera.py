"""Isolated Windows x64 input and lifecycle checks for the MMS camera adapter."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    root = Path(tempfile.mkdtemp(prefix='first-person-input-', dir=build.parent))
    cases = []
    for layout in ['legacy-mmdplugin', 'PLUGINS/legacy-mmdplugin']:
        for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
            directory = root / str(len(cases))
            directory.mkdir()
            exe = directory / name
            shutil.copy2(build / 'tests/Release/first_person_camera_tests.exe', exe)
            provider = directory / layout / 'MMDPlugin.dll'
            provider.parent.mkdir(parents=True)
            shutil.copy2(build / 'Release/legacy-mmdplugin/MMDPlugin.dll', provider)
            package = directory / 'PLUGINS/camera'
            package.mkdir(parents=True)
            dll = package / 'MMSFirstPersonCamera.dll'
            shutil.copy2(build / 'Release/optional-plugins/first-person-camera/MMSFirstPersonCamera.dll', dll)
            manifest = dict(schemaVersion=1, id='test.camera', name='FirstPersonCamera MMS', version='1.0.0',
                kind='legacy_mmdplugin', profile='first_person_camera_v4', entry=dll.name,
                sha256=hashlib.sha256(dll.read_bytes()).hexdigest(), architecture='x64', minWindows='6.1.0', reloadPolicy='restart')
            (package / 'plugin.json').write_text(json.dumps(manifest), encoding='utf-8')
            env = os.environ.copy()
            env['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
            run = subprocess.run([str(exe)], cwd=directory, env=env, capture_output=True, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW)
            (directory / 'result.log').write_bytes(run.stdout + run.stderr)
            case = dict(exe=name, layout=layout, exitCode=run.returncode, passed=run.returncode == 0, directory=str(directory))
            cases.append(case)
            (root / 'summary.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in cases) and len(cases)==6, cases=cases), indent=2), encoding='utf-8')
            print(json.dumps(case, ensure_ascii=True), flush=True)
            if run.returncode:
                print((run.stdout + run.stderr).decode('utf-8', errors='replace'))
                print(root / 'summary.json')
                return 1
    print(root / 'summary.json')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
