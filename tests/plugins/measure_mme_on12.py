"""Same-host DX9/On12 ABBA with unchanged original DLLs; measures the short validation scene."""
import argparse,ctypes as c,hashlib,json,os,shutil,statistics,subprocess,tempfile,time
from ctypes import wintypes as w
from pathlib import Path
import psutil
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--fixture',type=Path,required=True);a=p.parse_args()
b=a.build.resolve();root=Path(tempfile.mkdtemp(prefix='on12-abba-',dir=b.parent));folders={}
for mode in ['dx9','dx12']:
 folder=root/mode;shutil.copytree(a.fixture.resolve(),folder);folders[mode]=folder
 shutil.copy2(b/'tests/mme/Release/MikuMikuDance.exe',folder/'MikuMikuDance.exe')
 for dll in ['MMDFacade.dll','MMHack.dll']:shutil.copy2(b/'Release'/dll,folder/dll)
 shutil.copy2(b/'Release/PLUGINS/dx12mmepatch/dx12mmepatch.dll',folder/'PLUGINS/dx12mmepatch/dx12mmepatch.dll')
def hashes(folder):return {str(p.relative_to(folder)):hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*.dll')}
assert hashes(folders['dx9'])==hashes(folders['dx12'])
k=c.WinDLL('kernel32',use_last_error=True);k.GetProcessTimes.argtypes=[w.HANDLE]+[c.POINTER(w.FILETIME)]*4
rows=[]
for index,mode in enumerate(['dx9','dx12']+['dx9','dx12','dx12','dx9']*2):
 folder=folders[mode];trace=folder/'abba.log';trace.unlink(missing_ok=True)
 env={key:value for key,value in os.environ.items() if not key.startswith(('MMDFACADE_TEST_','MMS_TEST_'))}
 env.update(TEMP=str(folder/'temp'),TMP=str(folder/'temp'),LOCALAPPDATA=str(folder/'local'),MIKUMIKUSTUDIO_TRACE_FILE=str(trace),MMDFACADE_TEST_ORIGINAL_CAMERA='1')
 if mode=='dx12':env['MMDFACADE_TEST_REQUIRE_ON12']='1'
 start=time.perf_counter();startup=None;peak=0
 with (folder/f'sample-{index}.log').open('wb') as output:
  process=subprocess.Popen([str(folder/'MikuMikuDance.exe'),str(folder/'中文特效/block_simple.efk'),'1','--facade-real','--'+mode],cwd=folder,env=env,stdout=output,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
  probe=psutil.Process(process.pid)
  while process.poll() is None:
   try:peak=max(peak,probe.memory_info().rss)
   except psutil.Error:pass
   if startup is None and trace.exists():
    text=trace.read_text(encoding='utf-8',errors='replace')
    if 'started (first_person_camera_v4)' in text and 'started (effekseer_v2)' in text:startup=time.perf_counter()-start
   if time.perf_counter()-start>90:process.kill();raise RuntimeError('benchmark timeout')
   time.sleep(.02)
  times=[w.FILETIME() for _ in range(4)]
  if not k.GetProcessTimes(int(process._handle),*(c.byref(t) for t in times)):raise c.WinError(c.get_last_error())
 cpu=sum(((t.dwHighDateTime<<32)|t.dwLowDateTime)/1e7 for t in times[2:])
 text=trace.read_text(encoding='utf-8',errors='replace');expected='actual=12 reason=1' if mode=='dx12' else 'actual=9 reason=2'
 row=dict(backend=mode,warmup=index<2,startupSeconds=startup,scenarioSeconds=time.perf_counter()-start,cpuSeconds=cpu,sampledPeakWorkingSetMiB=peak/1048576,exitCode=process.returncode,backendConfirmed=expected in text)
 rows.append(row);print(json.dumps(row),flush=True);(root/'samples.json').write_text(json.dumps(rows,indent=2))
 if process.returncode or not row['backendConfirmed'] or startup is None:raise SystemExit(1)
summary={mode:{metric:statistics.median(r[metric] for r in rows if r['backend']==mode and not r['warmup']) for metric in ['startupSeconds','scenarioSeconds','cpuSeconds','sampledPeakWorkingSetMiB']} for mode in folders}
summary['pluginHashes']=hashes(folders['dx9']/'PLUGINS');(root/'summary.json').write_text(json.dumps(summary,indent=2));print(root/'summary.json')
