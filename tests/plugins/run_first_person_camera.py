"""Isolated Windows x64 input and lifecycle checks for the MMS camera adapter."""
import argparse
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
    for layout in ['PLUGINS/LEGACY', 'PLUGINS/legacy-mmdplugin', 'legacy-mmdplugin']:
        for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
            directory = root / str(len(cases))
            directory.mkdir()
            exe = directory / name
            shutil.copy2(build / 'tests/Release/first_person_camera_tests.exe', exe)
            provider = directory / layout / 'MMDPlugin.dll'
            provider.parent.mkdir(parents=True)
            shutil.copy2(build / 'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
            package_path = ['PLUGINS/camera', 'PLUGINS/optional-plugins/first-person-camera',
                            'PLUGINS/我的插件/自选相机'][len(cases) % 3]
            package = directory / package_path
            package.mkdir(parents=True)
            dll = package / 'MMSFirstPersonCamera.dll'
            shutil.copy2(build / 'Release/PLUGINS/first-person-camera/MMSFirstPersonCamera.dll', dll)
            shutil.copy2(build / 'Release/PLUGINS/first-person-camera/plugin.json', package / 'plugin.json')
            (directory / 'PLUGINS/config.json').write_text(json.dumps(dict(schemaVersion=1, maxDepth=3,
                disabledPlugins=[], enabledPlugins=['org.mikumikustudio.legacy.first-person-camera'])), encoding='utf-8')
            env = os.environ.copy()
            env['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
            run = subprocess.run([str(exe)], cwd=directory, env=env, capture_output=True, timeout=30,
                creationflags=subprocess.CREATE_NO_WINDOW)
            (directory / 'result.log').write_bytes(run.stdout + run.stderr)
            case = dict(exe=name, layout=layout, package=package_path, exitCode=run.returncode,
                        passed=run.returncode == 0, directory=str(directory))
            cases.append(case)
            (root / 'summary.json').write_text(json.dumps(dict(passed=all(c['passed'] for c in cases) and len(cases)==9, cases=cases), indent=2), encoding='utf-8')
            print(json.dumps(case, ensure_ascii=True), flush=True)
            if run.returncode:
                print((run.stdout + run.stderr).decode('utf-8', errors='replace'))
                print(root / 'summary.json')
                return 1
    print(root / 'summary.json')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
