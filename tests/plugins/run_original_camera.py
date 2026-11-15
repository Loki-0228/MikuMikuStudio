"""Run the original v0.01 image, never a rebuilt camera adapter, in isolated copies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SHA = 'd3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--automatic', action='store_true')
    args = parser.parse_args()
    build, original = args.build.resolve(), args.original.resolve()
    assert hashlib.sha256(original.read_bytes()).hexdigest() == SHA, 'unverified original input'
    root = Path(tempfile.mkdtemp(prefix='original-camera-', dir=build.parent))
    cases = []
    for layout in ['PLUGINS/LEGACY', 'PLUGINS/legacy-mmdplugin', 'legacy-mmdplugin', 'PLUGINS/components/bridge']:
        for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
            directory = root / str(len(cases))
            directory.mkdir()
            exe = directory / name
            shutil.copy2(build / 'tests/Release/original_camera_input_tests.exe', exe)
            facade = directory / ('PLUGINS/components/facade/MMDFacade.dll' if 'components' in layout else 'MMDFacade.dll')
            facade.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / 'Release/MMDFacade.dll', facade)
            provider = directory / layout / 'MMDPlugin.dll'
            provider.parent.mkdir(parents=True)
            shutil.copy2(build / 'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
            package = directory / ['PLUGINS/LEGACY/camera', 'PLUGINS/LEGACY/分组/相机', 'PLUGINS/LEGACY/相机'][len(cases) % 3]
            package.mkdir(parents=True)
            dll = package / original.name
            shutil.copy2(original, dll)
            if not args.automatic:
                manifest = dict(schemaVersion=1, id='test.original.camera', name='Original FirstPersonCamera',
                    version='0.1.0', kind='legacy_mmdplugin', profile='first_person_camera_v4',
                    entry=dll.name, sha256=SHA, architecture='x64', minWindows='6.1.0', reloadPolicy='restart')
                (package / 'plugin.json').write_text(json.dumps(manifest), encoding='utf-8')
            environment = os.environ.copy()
            environment['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
            run = subprocess.run([str(exe)], cwd=directory, env=environment, capture_output=True,
                timeout=40, creationflags=subprocess.CREATE_NO_WINDOW)
            (directory / 'result.log').write_bytes(run.stdout + run.stderr)
            cached = list((directory / 'data/plugins/legacy_mmdplugin').glob('**/' + original.name))
            unchanged = hashlib.sha256(dll.read_bytes()).hexdigest() == SHA and len(cached) == 1 and hashlib.sha256(cached[0].read_bytes()).hexdigest() == SHA
            case = dict(exe=name, provider=layout, package=str(package.relative_to(directory)),
                automatic=args.automatic, exitCode=run.returncode, originalAndCacheUnchanged=unchanged,
                passed=run.returncode == 0 and unchanged, directory=str(directory))
            cases.append(case)
            (root / 'summary.json').write_text(json.dumps(dict(passed=len(cases) == 12 and all(c['passed'] for c in cases), cases=cases), indent=2), encoding='utf-8')
            print(json.dumps(case, ensure_ascii=True), flush=True)
            if not case['passed']:
                print((run.stdout + run.stderr).decode('utf-8', errors='replace'))
                print(root / 'summary.json')
                return 1
    print(root / 'summary.json')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
