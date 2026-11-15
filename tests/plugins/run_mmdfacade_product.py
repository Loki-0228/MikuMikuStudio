"""Opt-in hidden product smoke with a locally built MMDUtility and optional MME."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from host_smoke import windows, send, u


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--utility', type=Path, required=True)
    parser.add_argument('--mme', type=Path)
    parser.add_argument('--backend', choices=['dx9', 'dx12'])
    parser.add_argument('--facade-state', choices=['present', 'missing', 'old-name-only'], default='present')
    args = parser.parse_args()
    build = args.build.resolve()
    root = Path(tempfile.mkdtemp(prefix='mmdfacade-product-', dir=build.parent))
    cases = []
    u.GetMenuStringW.argtypes = [w.HMENU, w.UINT, w.LPWSTR, c.c_int, w.UINT]
    for layout in ['PLUGINS/LEGACY', 'PLUGINS/legacy-mmdplugin', 'legacy-mmdplugin']:
        for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
            directory = root / str(len(cases))
            directory.mkdir()
            exe = directory / name
            shutil.copy2(build / 'Release/MikuMikuDance.exe', exe)
            for dll in ['MMDFacade.dll', 'MMHack.dll']:
                if dll == 'MMDFacade.dll' and args.facade_state == 'missing':
                    continue
                destination = 'MMSAdapter.dll' if dll == 'MMDFacade.dll' and args.facade_state == 'old-name-only' else dll
                shutil.copy2(build / 'Release' / dll, directory / destination)
            provider = directory / layout / 'MMDPlugin.dll'
            provider.parent.mkdir(parents=True)
            shutil.copy2(build / 'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
            package = directory / 'PLUGINS/utility'
            package.mkdir(parents=True)
            shutil.copy2(args.utility, package / 'MMDUtility.dll')
            manifest = dict(schemaVersion=1, id='test.utility', name='MMDUtility', version='1.0.0',
                kind='legacy_mmdplugin', profile='window_observer_v3', entry='MMDUtility.dll',
                sha256=hashlib.sha256(args.utility.read_bytes()).hexdigest(),
                architecture='x64', minWindows='6.1.0', reloadPolicy='restart')
            (package / 'plugin.json').write_text(json.dumps(manifest), encoding='utf-8')
            if args.mme:
                package = directory / 'PLUGINS/mme'
                package.mkdir()
                shutil.copy2(args.mme / 'MMEffect.dll', package / 'MMEffect.dll')
                (package / 'plugin.json').write_text(json.dumps(dict(schemaVersion=1, id='test.mme', name='MME',
                    version='0.37.0', kind='legacy_mme', entry='MMEffect.dll', architecture='x64',
                    minWindows='6.1.0', reloadPolicy='restart')), encoding='utf-8')
            (directory / 'PLUGINS/config.json').write_text(json.dumps(dict(schemaVersion=1,maxDepth=3,disabledPlugins=[])))
            (directory / 'data').mkdir()
            (directory / 'data/settings.json').write_text(json.dumps(dict(schemaVersion=1,
                sections=dict(processor=dict(vertices=dict(mode='cpu'))))), encoding='utf-8')
            env = os.environ.copy()
            env['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            command = [str(exe)] + (['--' + args.backend] if args.backend else [])
            process = subprocess.Popen(command, cwd=directory, env=env, startupinfo=startup)
            case = dict(backendRequested=args.backend, exe=name, layout=layout, directory=str(directory), passed=False, terminated=False)
            main_window = None
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline and process.poll() is None:
                    main_window = next((h for h, cls in windows(process.pid) if cls == 'Polygon Movie Maker'), None)
                    trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace') if (directory / 'runtime.log').exists() else ''
                    if main_window and 'test.utility started (window_observer_v3)' in trace:
                        break
                    time.sleep(0.1)
                else:
                    raise RuntimeError('product startup / observer timeout')
                send(main_window, 0)
                menu = u.GetMenu(main_window)
                titles = []
                for index in range(u.GetMenuItemCount(menu)):
                    title = c.create_unicode_buffer(256)
                    u.GetMenuStringW(menu, index, title, len(title), 0x400)
                    titles.append(title.value)
                if 'MMDUtility' not in titles:
                    raise RuntimeError(f'upstream menu absent: {titles}')
                case['menuPresent'] = True
                if args.mme:
                    startup_log = directory / 'data/plugins/legacy_mme/startup.log'
                    mme_trace = startup_log.read_text(encoding='utf-8', errors='replace') if startup_log.exists() else ''
                    if args.facade_state == 'present':
                        if 'test.mme loaded from' not in mme_trace:
                            raise RuntimeError('MME package did not load: ' + mme_trace)
                        case['mmeLoaded'] = True
                    else:
                        if 'LEGACY_MME missing file or reparse point:' not in trace or 'MMDFacade.dll' not in trace:
                            raise RuntimeError('missing new facade has no explicit diagnostic')
                        case['missingFacadeRejected'] = True
                send(main_window, 0x10)
                case['exitCode'] = process.wait(timeout=20)
                trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace')
                case['passed'] = case['exitCode'] == 0 and 'stopped and destroyed (test.utility)' in trace
            except Exception as error:
                case['error'] = str(error)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                    case['terminated'] = True
            cases.append(case)
            print(json.dumps(case, ensure_ascii=True), flush=True)
            if not case['passed']:
                break
        if not cases[-1]['passed']:
            break
    report = dict(passed=len(cases)==9 and all(c['passed'] for c in cases), cases=cases,
                  utilitySha256=hashlib.sha256(args.utility.read_bytes()).hexdigest())
    (root / 'summary.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(root / 'summary.json')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
