"""Windows x64 Release callback matrix; uses isolated copies, never a user install."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    root = Path(tempfile.mkdtemp(prefix='mmdfacade-callbacks-', dir=build.parent))
    cases = []
    modes = ['enabled', 'disabled', 'missing', 'unsupported', 'wrong-version', 'throw', 'post-throw', 'stop']
    for mode in modes:
        for layout in ['PLUGINS/LEGACY', 'PLUGINS/legacy-mmdplugin', 'legacy-mmdplugin']:
            for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
                directory = root / str(len(cases))
                directory.mkdir()
                exe = directory / name
                shutil.copy2(build / 'tests/Release/legacy_callback_tests.exe', exe)
                if mode != 'missing':
                    provider = directory / layout / 'MMDPlugin.dll'
                    provider.parent.mkdir(parents=True)
                    shutil.copy2(build / 'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
                ids = []
                for abi, dll, profile in [(2, 'reset.dll', 'device_reset_v2'), (3, 'observer.dll', 'window_observer_v3')]:
                    package = directory / 'PLUGINS' / f'fixture{abi}'
                    package.mkdir(parents=True)
                    shutil.copy2(build / f'tests/plugins/callbacks/Release/legacy_callback_fixture_v{abi}.dll', package / dll)
                    identity = f'test.fixture{abi}'
                    ids.append(identity)
                    manifest = dict(schemaVersion=1, id=identity, name=identity, version='1.0.0',
                                    kind='legacy_mmdplugin', profile=profile if mode != 'unsupported' else 'full_abi4',
                                    entry=dll, sha256=hashlib.sha256((package / dll).read_bytes()).hexdigest(),
                                    architecture='x64', minWindows='6.1.0', reloadPolicy='restart')
                    (package / 'plugin.json').write_text(json.dumps(manifest), encoding='utf-8')
                (directory / 'PLUGINS/config.json').write_text(json.dumps(dict(schemaVersion=1, maxDepth=3,
                    disabledPlugins=ids if mode == 'disabled' else [])), encoding='utf-8')
                import os
                env = os.environ.copy()
                env.pop('MMDFACADE_TEST_WRONG_VERSION', None)
                env.pop('MMDFACADE_TEST_THROW', None)
                env.pop('MMDFACADE_TEST_POST_THROW', None)
                if mode == 'wrong-version':
                    env['MMDFACADE_TEST_WRONG_VERSION'] = '1'
                env['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
                run = subprocess.run([str(exe), mode], cwd=directory, env=env, capture_output=True,
                                     timeout=45, creationflags=subprocess.CREATE_NO_WINDOW)
                (directory / 'result.log').write_bytes(run.stdout + run.stderr)
                trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace') if (directory / 'runtime.log').exists() else ''
                required = {'missing': 'startup failed', 'unsupported': 'unsupported legacy MMDPlugin profile',
                            'wrong-version': 'version does not match'}.get(mode)
                passed = run.returncode == 0 and (not required or required in trace)
                cases.append(dict(mode=mode, layout=layout, exe=name, exitCode=run.returncode,
                                  passed=bool(passed), directory=str(directory)))
                print(mode, layout, name, passed, flush=True)
                if not passed:
                    print((run.stdout + run.stderr).decode('utf-8', errors='replace'), trace[-2000:], flush=True)
                    break
            if not cases[-1]['passed']:
                break
        if not cases[-1]['passed']:
            break
    report = dict(passed=len(cases) == len(modes) * 9 and all(c['passed'] for c in cases), cases=cases)
    (root / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(root / 'summary.json')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
