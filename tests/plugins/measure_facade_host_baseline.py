"""Compare unchanged abi production code with the current host using identical plugin bytes.

Both hosts use the source camera adapter: the abi baseline cannot run the original
camera. This benchmark does not count as an original-DLL compatibility test.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time
import psutil
from run_mmdfacade_candidates import observer_manifest

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--baseline-build', type=Path, required=True)
parser.add_argument('--fixture', type=Path, required=True)
args = parser.parse_args()
build, baseline, fixture = args.build.resolve(), args.baseline_build.resolve(), args.fixture.resolve()
root = Path(tempfile.mkdtemp(prefix='facade-abba-', dir=build.parent))
folders = {}
for name, binaries in [('baseline', baseline), ('current', build)]:
    directory = root / name; directory.mkdir(); folders[name] = directory
    shutil.copy2(binaries/'tests/mme/Release/MikuMikuDance.exe', directory/'MikuMikuDance.exe')
    for dll in ['MMDFacade.dll','MMHack.dll']: shutil.copy2(build/'Release'/dll, directory/dll)
    provider = directory/'PLUGINS/LEGACY/MMDPlugin.dll'; provider.parent.mkdir(parents=True)
    shutil.copy2(build/'Release/PLUGINS/LEGACY/MMDPlugin.dll', provider)
    for identity, source, profile in [('test.camera', build/'Release/PLUGINS/first-person-camera/MMSFirstPersonCamera.dll','first_person_camera_v4'),
            ('test.effekseer', fixture/'PLUGINS/LEGACY/test.effekseer/EffekseerForMMD.dll','effekseer_v2')]:
        package = directory/'PLUGINS/LEGACY'/identity; shutil.copytree(source.parent, package)
        (package/'plugin.json').write_text(json.dumps(observer_manifest(source, identity, profile)), encoding='utf-8')
    mme = directory/'PLUGINS/LEGACY/mme'; shutil.copytree(fixture/'PLUGINS/LEGACY/mme', mme)
    (mme/'plugin.json').write_text(json.dumps(dict(schemaVersion=1,id='test.mme',name='MME',version='0.37.0',kind='legacy_mme',
        entry='MMEffect.dll',architecture='x64',minWindows='6.1.0',reloadPolicy='restart')), encoding='utf-8')
    for folder in ['data','temp','local']: (directory/folder).mkdir()
    shutil.copy2(fixture/'data/settings.json', directory/'data/settings.json')
    shutil.copytree(fixture/'中文特效', directory/'effect')

def hashes(folder):
    return {str(file.relative_to(folder)):hashlib.sha256(file.read_bytes()).hexdigest() for file in folder.rglob('*.dll')}
assert hashes(folders['baseline']) == hashes(folders['current']), 'plugin bytes differ'
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
def ticks(value): return ((value.dwHighDateTime << 32) | value.dwLowDateTime) / 1e7
rows = []
for index, name in enumerate(['baseline','current'] + ['baseline','current','current','baseline']*2):
    directory = folders[name]; trace = directory/'measure.log'; trace.unlink(missing_ok=True)
    env = os.environ.copy()
    for key in ['MMDFACADE_TEST_ORIGINAL_CAMERA','MMDFACADE_TEST_SENDINPUT','MMDFACADE_TEST_NO_MME','MMDFACADE_TEST_REQUIRE_ON12']: env.pop(key,None)
    env.update(TEMP=str(directory/'temp'),TMP=str(directory/'temp'),LOCALAPPDATA=str(directory/'local'),MIKUMIKUSTUDIO_TRACE_FILE=str(trace))
    start, startup, peak = time.perf_counter(), None, 0
    with (directory/f'measure-{index}.log').open('wb') as output:
        process = subprocess.Popen([str(directory/'MikuMikuDance.exe'),str(directory/'effect/block_simple.efk'),'1','--facade-real','--dx9'],
            cwd=directory,env=env,stdout=output,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        probe = psutil.Process(process.pid)
        while process.poll() is None:
            try: peak = max(peak,probe.memory_info().rss)
            except psutil.Error: pass
            if startup is None and trace.exists():
                text = trace.read_text(encoding='utf-8',errors='replace')
                if 'test.camera started (first_person_camera_v4)' in text and 'test.effekseer started (effekseer_v2)' in text:
                    startup = time.perf_counter()-start
            if time.perf_counter()-start > 90: process.kill(); raise RuntimeError('benchmark timeout')
            time.sleep(.02)
        times = [w.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(int(process._handle),*(c.byref(t) for t in times)): raise c.WinError(c.get_last_error())
    row = dict(host=name,warmup=index<2,startupSeconds=startup,scenarioSeconds=time.perf_counter()-start,
        cpuSeconds=ticks(times[2])+ticks(times[3]),sampledPeakWorkingSetMiB=peak/1048576,exitCode=process.returncode)
    rows.append(row); print(json.dumps(row),flush=True)
    (root/'samples.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
    if process.returncode or startup is None: raise SystemExit(1)
summary = {name:{metric:statistics.median(row[metric] for row in rows if row['host']==name and not row['warmup'])
    for metric in ['startupSeconds','scenarioSeconds','cpuSeconds','sampledPeakWorkingSetMiB']} for name in folders}
summary['sharedPluginHashes'] = hashes(folders['baseline']/'PLUGINS')
(root/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(root/'summary.json')
