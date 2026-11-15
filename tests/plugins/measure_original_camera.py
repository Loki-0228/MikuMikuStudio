"""Same-host ABBA comparison of original camera versus source adapter; not a host-baseline benchmark."""
import argparse
import ctypes as c
from ctypes import wintypes as w
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
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='facade-cost-', dir=args.build.resolve().parent))
folders = {}
for name in ['original', 'adapter']:
    folder = root / name
    shutil.copytree(args.fixture, folder)
    folders[name] = folder
    if name == 'adapter':
        camera = folder / 'PLUGINS/LEGACY/test.camera'
        (camera / 'MMDFirstPersonCamera.dll').unlink()
        dll = camera / 'MMSFirstPersonCamera.dll'
        shutil.copy2(args.build / 'Release/PLUGINS/first-person-camera/MMSFirstPersonCamera.dll', dll)
        (camera / 'plugin.json').write_text(json.dumps(observer_manifest(dll, 'test.camera', 'first_person_camera_v4')), encoding='utf-8')
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
def ticks(value): return ((value.dwHighDateTime << 32) | value.dwLowDateTime) / 1e7
rows = []
# Each installation gets an excluded warmup, then two interleaved ABBA blocks.
for index, name in enumerate(['original', 'adapter'] + ['original', 'adapter', 'adapter', 'original'] * 2):
    folder = folders[name]
    trace = folder / 'measure.log'
    trace.unlink(missing_ok=True)
    env = os.environ.copy()
    env.update(TEMP=str(folder / 'temp'), TMP=str(folder / 'temp'), LOCALAPPDATA=str(folder / 'local'), MIKUMIKUSTUDIO_TRACE_FILE=str(trace))
    if name == 'original': env['MMDFACADE_TEST_ORIGINAL_CAMERA'] = '1'
    else: env.pop('MMDFACADE_TEST_ORIGINAL_CAMERA', None)
    start, startup, peak = time.perf_counter(), None, 0
    with (folder / f'measure-{index}.log').open('wb') as output:
        process = subprocess.Popen([str(folder / 'MikuMikuDance.exe'), str(folder / '中文特效/block_simple.efk'), '1', '--facade-real'],
            cwd=folder, env=env, stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        probe = psutil.Process(process.pid)
        while process.poll() is None:
            try: peak = max(peak, probe.memory_info().rss)
            except psutil.Error: pass
            if startup is None and trace.exists() and 'started (first_person_camera_v4)' in trace.read_text(encoding='utf-8', errors='replace'):
                startup = time.perf_counter() - start
            if time.perf_counter() - start > 90: process.kill(); raise RuntimeError('benchmark timeout')
            time.sleep(.02)
        times = [w.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(int(process._handle), *(c.byref(t) for t in times)): raise c.WinError(c.get_last_error())
    row = dict(kind=name, warmup=index < 2, startupSeconds=startup, scenarioSeconds=time.perf_counter()-start,
        cpuSeconds=ticks(times[2])+ticks(times[3]), sampledPeakWorkingSetMiB=peak/1048576, exitCode=process.returncode)
    rows.append(row); print(json.dumps(row), flush=True)
    (root / 'samples.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    if process.returncode: raise SystemExit(1)
summary = {name: {metric: statistics.median(row[metric] for row in rows if row['kind'] == name and not row['warmup'])
    for metric in ['startupSeconds', 'scenarioSeconds', 'cpuSeconds', 'sampledPeakWorkingSetMiB']} for name in folders}
(root / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(root / 'summary.json')
