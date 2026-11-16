"""Isolated integration controls for the real Patch4MMS/MME bridge."""
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--build',type=Path,required=True)
p.add_argument('--fixture',type=Path,required=True)
p.add_argument('--old-bridge',type=Path,required=True)
p.add_argument('--only')
a=p.parse_args(); b=a.build.resolve(); source=a.fixture.resolve()
root=Path(tempfile.mkdtemp(prefix='on12-controls-',dir=b.parent)); results=[]
for case,backend,reason in [('dx9',9,2),('dx9-first',9,2),('dx9-last',9,2),('disabled',9,3),
        ('uninstalled',9,None),('old-bridge',9,5),('factory-failure',9,6),('device-failure',9,7),('missing-mme',0,4),('moved',12,1),
        ('corrupt-plugin',None,None),('missing-entry',None,None),('missing-service',None,None)]:
    if a.only and case!=a.only: continue
    folder=root/str(len(results)); shutil.copytree(source,folder)
    shutil.copy2(b/'tests/mme/Release/MikuMikuDance.exe',folder/'MikuMikuDance.exe')
    for name in ['MMDFacade.dll','MMHack.dll']: shutil.copy2(b/'Release'/name,folder/name)
    package=folder/'PLUGINS/dx12mmepatch'
    for name in ['dx12mmepatch.dll','plugin.json']: shutil.copy2(b/'Release/PLUGINS/dx12mmepatch'/name,package/name)
    config=folder/'PLUGINS/config.json'; settings=json.loads(config.read_text())
    flags=['--dx12']
    if case=='dx9': flags=['--dx9']
    if case=='dx9-first': flags=['--dx9','--dx12']
    if case=='dx9-last': flags=['--dx12','--dx9']
    if case=='disabled': settings['disabledPlugins']=['patch4mms']
    if case in ['uninstalled','moved']:
        destination=folder/'uninstalled' if case=='uninstalled' else folder/'PLUGINS/分组/补丁'
        assert package.resolve().is_relative_to(folder.resolve()) and destination.resolve().is_relative_to(folder.resolve())
        destination.parent.mkdir(parents=True,exist_ok=True); package.rename(destination)
    if case=='old-bridge': shutil.copy2(a.old_bridge,folder/'MMHack.dll')
    if case=='missing-mme': (folder/'PLUGINS/LEGACY/mme/MMEffect.dll').unlink()
    if case=='corrupt-plugin': (package/'dx12mmepatch.dll').write_bytes(b'Invalid DLL')
    if case=='missing-entry': (package/'dx12mmepatch.dll').unlink()
    if case=='missing-service':
        manifest=json.loads((package/'plugin.json').read_text()); manifest['requiresServices']=[]
        (package/'plugin.json').write_text(json.dumps(manifest))
    config.write_text(json.dumps(settings))
    trace=folder/'control-runtime.log'
    env={key:value for key,value in os.environ.items() if not key.startswith(('MMDFACADE_TEST_','MMS_TEST_'))}
    env.update(TEMP=str(folder/'temp'),TMP=str(folder/'temp'),LOCALAPPDATA=str(folder/'local'),
        MIKUMIKUSTUDIO_TRACE_FILE=str(trace),MMDFACADE_TEST_ORIGINAL_CAMERA='1')
    if case=='missing-mme': env['MMDFACADE_TEST_NO_MME']='1'
    if case in ['factory-failure','device-failure']: env['MMDFACADE_TEST_ON12_FAILURE']=case.split('-')[0]
    originals={str(f.relative_to(folder)):hashlib.sha256(f.read_bytes()).hexdigest() for f in (folder/'PLUGINS/LEGACY').rglob('*.dll')}
    with (folder/'control.log').open('wb') as out:
        run=subprocess.run([str(folder/'MikuMikuDance.exe'),str(folder/'中文特效/block_simple.efk'),'1','--facade-real']+flags,
            cwd=folder,env=env,stdout=out,stderr=subprocess.STDOUT,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
    log=(folder/'control.log').read_text(encoding='utf-8',errors='replace')
    text=trace.read_text(encoding='utf-8',errors='replace') if trace.exists() else ''
    status=re.search(r'Patch4MMS requested=(\d+) actual=(\d+) reason=(\d+)',text)
    unchanged=all(hashlib.sha256((folder/name).read_bytes()).hexdigest()==digest for name,digest in originals.items())
    passed=run.returncode==0 and unchanged
    if backend is None: passed=run.returncode==8 and 'MMDFacade' in log and ('patch4mms' in log.lower() or 'Patch4MMS' in log)
    elif reason is not None: passed &= status is not None and int(status[2])==backend and int(status[3])==reason
    else: passed &= 'mmeDeviceOn12=80004002' in log
    if case=='disabled': passed=run.returncode==0 and unchanged and 'no enabled mme-d3d9on12 startup request' in log and status is None
    if case in ['factory-failure','device-failure']: passed &= 'INJECT On12' in log
    row=dict(case=case,passed=bool(passed),exitCode=run.returncode,directory=str(folder),originalsUnchanged=unchanged,
        actualBackend=int(status[2]) if status else None,reason=int(status[3]) if status else None)
    results.append(row); print(json.dumps(row),flush=True)
    (root/'summary.json').write_text(json.dumps(results,indent=2))
    if not passed: print(log[-2500:].encode('ascii','backslashreplace').decode()); break
print(root/'summary.json'); raise SystemExit(0 if all(r['passed'] for r in results) else 1)
