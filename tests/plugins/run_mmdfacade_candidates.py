"""Opt-in product tests for real plugins currently rejected by MMDFacade.

Passing means the rejection, disabled state and host controls behaved correctly.
It does not mean the candidate's rendering or input callbacks are supported.
"""
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
from pe_audit import inspect


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding='utf-8')


def modules(process):
    api = c.WinDLL('psapi', use_last_error=True)
    api.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
    api.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
    handles, needed = (w.HMODULE * 4096)(), w.DWORD()
    if not api.EnumProcessModulesEx(int(process._handle), handles, c.sizeof(handles), c.byref(needed), 3):
        raise c.WinError(c.get_last_error())
    if needed.value > c.sizeof(handles):
        raise RuntimeError('module inventory overflow')
    result = []
    for handle in handles[:needed.value // c.sizeof(w.HMODULE)]:
        path = c.create_unicode_buffer(32768)
        if not api.GetModuleFileNameExW(int(process._handle), handle, path, len(path)):
            raise c.WinError(c.get_last_error())
        result.append(path.value)
    return result


def observer_manifest(path, plugin_id, profile):
    return dict(schemaVersion=1, id=plugin_id, name=path.stem, version='1.0.0',
        kind='legacy_mmdplugin', profile=profile, entry=path.name, sha256=digest(path),
        architecture='x64', minWindows='6.1.0', reloadPolicy='restart')


def run_case(args, directory, layout, name, mode):
    directory.mkdir()
    exe = directory / name
    shutil.copy2(args.release / 'MikuMikuDance.exe', exe)
    for dll in ['MMDFacade.dll', 'MMHack.dll']:
        shutil.copy2(args.release / dll, directory / dll)
    provider = directory / layout / 'MMDPlugin.dll'
    provider.parent.mkdir(parents=True)
    shutil.copy2(args.release / 'legacy-mmdplugin/MMDPlugin.dll', provider)
    candidate = directory / 'PLUGINS/candidate'
    shutil.copytree(args.candidate.parent, candidate)
    profile = args.unsupported_profile if mode == 'unsupported-profile' else args.profile
    write_json(candidate / 'plugin.json', observer_manifest(args.candidate, 'test.candidate', profile))
    if mode == 'missing':
        (candidate / args.candidate.name).unlink()
    utility = directory / 'PLUGINS/utility'
    utility.mkdir()
    shutil.copy2(args.utility, utility / args.utility.name)
    write_json(utility / 'plugin.json', observer_manifest(args.utility, 'test.utility', 'window_observer_v3'))
    mme = directory / 'PLUGINS/mme'
    mme.mkdir()
    shutil.copy2(args.mme / 'MMEffect.dll', mme / 'MMEffect.dll')
    write_json(mme / 'plugin.json', dict(schemaVersion=1, id='test.mme', name='MME',
        version='0.37.0', kind='legacy_mme', entry='MMEffect.dll', architecture='x64',
        minWindows='6.1.0', reloadPolicy='restart'))
    write_json(directory / 'PLUGINS/config.json', dict(schemaVersion=1, maxDepth=3,
        disabledPlugins=['test.candidate'] if mode == 'disabled' else []))
    (directory / 'data').mkdir()
    write_json(directory / 'data/settings.json', dict(schemaVersion=1,
        sections=dict(processor=dict(vertices=dict(mode='cpu')))))
    environment = os.environ.copy()
    environment['MIKUMIKUSTUDIO_TRACE_FILE'] = str(directory / 'runtime.log')
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    case = dict(exe=name, layout=layout, mode=mode, directory=str(directory), checksPassed=False, terminated=False)
    with (directory / 'process.log').open('w', encoding='utf-8') as output:
        process = subprocess.Popen([str(exe)], cwd=directory, env=environment,
            startupinfo=startup, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline and process.poll() is None:
                main = next((h for h, cls in windows(process.pid) if cls == 'Polygon Movie Maker'), None)
                trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace') if (directory / 'runtime.log').exists() else ''
                if main and 'test.utility started (window_observer_v3)' in trace:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError('product startup / control observer timeout')
            send(main, 0)
            menu, titles = u.GetMenu(main), []
            for index in range(u.GetMenuItemCount(menu)):
                title = c.create_unicode_buffer(256)
                u.GetMenuStringW(menu, index, title, len(title), 0x400)
                titles.append(title.value)
            if 'MMDUtility' not in titles:
                raise RuntimeError('control observer menu absent')
            paths = modules(process)
            write_json(directory / 'modules.json', paths)
            mapped = [p for p in paths if Path(p).name.lower() == args.candidate.name.lower()]
            accepting = args.accept and mode == 'enabled'
            if bool(mapped) != accepting:
                raise RuntimeError('candidate mapping differs from expected state')
            if [Path(p) for p in paths if Path(p).name.lower() == 'mmdplugin.dll'] != [provider]:
                raise RuntimeError('product loaded a different MMDPlugin provider')
            if not any(Path(p).name.lower() == 'mmdfacade.dll' for p in paths):
                raise RuntimeError('MMDFacade module absent')
            mme_trace = (directory / 'data/plugins/legacy_mme/startup.log').read_text(encoding='utf-8', errors='replace')
            if 'test.mme loaded from' not in mme_trace:
                raise RuntimeError('MME control did not load')
            trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace')
            candidate_lines = [line for line in trace.splitlines() if 'LEGACY_MMD ' in line and ('test.candidate' in line or 'PLUGINS\\candidate' in line)]
            if not accepting and ('test.candidate started' in trace or 'test.candidate loaded' in trace):
                raise RuntimeError('unexpected candidate load or startup')
            if accepting:
                if f'test.candidate started ({args.profile})' not in trace:
                    raise RuntimeError('candidate factory did not complete')
                case['loadedImage'] = inspect(Path(mapped[0]))
                if args.profile == 'effekseer_v2' and 'Effekseer' not in titles:
                    raise RuntimeError('real Effekseer menu absent')
            expected = {'rejected': args.expect, 'missing': 'missing file or reparse point:',
                        'unsupported-profile': 'unsupported legacy MMDPlugin profile'}.get(mode)
            if expected and not any(expected in line for line in candidate_lines):
                raise RuntimeError('expected diagnostic absent: ' + str(candidate_lines))
            if mode == 'disabled' and candidate_lines:
                raise RuntimeError('disabled candidate reached legacy startup')
            case.update(diagnostics=candidate_lines, candidateMapped=bool(mapped), observerMenuPresent=True, mmeLoaded=True)
            send(main, 0x10)
            case['exitCode'] = process.wait(timeout=20)
            trace = (directory / 'runtime.log').read_text(encoding='utf-8', errors='replace')
            if case['exitCode'] != 0 or 'stopped and destroyed (test.utility)' not in trace:
                raise RuntimeError('host / control observer did not shut down cleanly')
            if accepting and 'stopped and destroyed (test.candidate)' not in trace:
                raise RuntimeError('candidate was not destroyed cleanly')
            if mode != 'missing' and digest(candidate / args.candidate.name) != digest(args.candidate):
                raise RuntimeError('candidate bytes changed')
            case['checksPassed'] = True
        except Exception as error:
            case['error'] = str(error)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
                case['terminated'] = True
    return case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ['release', 'candidate', 'utility', 'mme']:
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--profile', choices=['device_reset_v2', 'window_observer_v3', 'effekseer_v2', 'first_person_camera_v4'], required=True)
    parser.add_argument('--expect')
    parser.add_argument('--accept', action='store_true', help='Require a real factory and clean destruction, without claiming functional support')
    parser.add_argument('--unsupported-profile')
    args = parser.parse_args()
    if not args.accept and not args.expect:
        parser.error('--expect is required for rejection tests')
    for option in ['release', 'candidate', 'utility', 'mme']:
        setattr(args, option, getattr(args, option).resolve())
    u.GetMenuStringW.argtypes = [w.HMENU, w.UINT, w.LPWSTR, c.c_int, w.UINT]
    root = Path(tempfile.mkdtemp(prefix='mmdfacade-candidates-', dir=args.release.parent))
    modes = ['enabled' if args.accept else 'rejected', 'disabled', 'missing'] + (['unsupported-profile'] if args.unsupported_profile else [])
    report = dict(label=args.label, candidate=inspect(args.candidate), checksPassed=False,
        pluginUsable=False, renderingTested=False, inputTested=False,
        hostSha256=digest(args.release / 'MikuMikuDance.exe'), cases=[])
    for layout in ['legacy-mmdplugin', 'PLUGINS/legacy-mmdplugin']:
        for name in ['MikuMikuDance.exe', 'Renamed Studio.exe', '中文工作室.exe']:
            for mode in modes:
                case = run_case(args, root / str(len(report['cases'])), layout, name, mode)
                report['cases'].append(case)
                write_json(root / 'summary.json', report)
                print(json.dumps(case, ensure_ascii=True), flush=True)
                if not case['checksPassed']:
                    print(root / 'summary.json')
                    return 1
    report['sourceUnchanged'] = digest(args.candidate) == report['candidate']['sha256']
    report['checksPassed'] = report['sourceUnchanged'] and all(case['checksPassed'] for case in report['cases'])
    write_json(root / 'summary.json', report)
    print(root / 'summary.json')
    return 0 if report['checksPassed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
