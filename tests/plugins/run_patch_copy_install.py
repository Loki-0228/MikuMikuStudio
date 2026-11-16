"""Verify copy-only startup with original plugins and unchanged user configuration."""
import argparse,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--build',type=Path,required=True)
p.add_argument('--fixture',type=Path,required=True)
p.add_argument('--package',type=Path,required=True)
p.add_argument('--only',choices=['copy','existing-config','old-enabled','old-blacklist','optin-disabled','optin-enabled','dx9','grouped','missing-service'])
a=p.parse_args();b=a.build.resolve();root=Path(tempfile.mkdtemp(prefix='patch-copy-',dir=b.parent));rows=[]
cases=[('copy',12),('existing-config',12),('old-enabled',12),('old-blacklist',9),('optin-disabled',9),('optin-enabled',12),('dx9',9),('grouped',12),('missing-service',None)]
for case,backend in cases:
 if a.only and a.only!=case:continue
 folder=root/str(len(rows));shutil.copytree(a.fixture.resolve(),folder,ignore=shutil.ignore_patterns('patch4mms','dx12mmepatch'))
 shutil.copy2(b/'tests/mme/Release/MikuMikuDance.exe',folder/'MikuMikuDance.exe')
 for dll in ['MMDFacade.dll','MMHack.dll']:shutil.copy2(b/'Release'/dll,folder/dll)
 package=folder/('PLUGINS/中文分组/dx12mmepatch' if case=='grouped' else 'PLUGINS/dx12mmepatch')
 shutil.copytree(a.package.resolve(),package)
 config=folder/'PLUGINS/config.json';config.unlink(missing_ok=True)
 if case in ['existing-config','old-enabled','old-blacklist','optin-enabled']:
  value=dict(schemaVersion=1,maxDepth=2,disabledPlugins=['other.disabled'],custom=dict(preserve='value'))
  if case in ['old-enabled','optin-enabled']:value['enabledPlugins']=['patch4mms']
  if case=='old-blacklist':value['disabledPlugins'].append('patch4mms')
  config.write_text(json.dumps(value),encoding='utf-8')
 if case.startswith('optin-') or case=='missing-service':
  manifest=json.loads((package/'plugin.json').read_text(encoding='utf-8'))
  if case.startswith('optin-'):manifest['optIn']=True
  else:manifest['requiresServices']=[]
  (package/'plugin.json').write_text(json.dumps(manifest),encoding='utf-8')
 before=config.read_bytes() if config.exists() else None
 originals={str(f.relative_to(folder)):hashlib.sha256(f.read_bytes()).hexdigest() for f in (folder/'PLUGINS/LEGACY').rglob('*.dll')}
 env={key:value for key,value in os.environ.items() if not key.startswith(('MMDFACADE_TEST_','MMS_TEST_'))}
 trace=folder/'copy-runtime.log'
 env.update(TEMP=str(folder/'temp'),TMP=str(folder/'temp'),LOCALAPPDATA=str(folder/'local'),MIKUMIKUSTUDIO_TRACE_FILE=str(trace),MMDFACADE_TEST_ORIGINAL_CAMERA='1')
 if backend==12:env['MMDFACADE_TEST_REQUIRE_ON12']='1'
 with (folder/'copy.log').open('wb') as log:
  result=subprocess.run([str(folder/'MikuMikuDance.exe'),str(folder/'中文特效/block_simple.efk'),'1','--facade-real','--dx9' if case=='dx9' else '--dx12'],cwd=folder,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
 text=trace.read_text(encoding='utf-8',errors='replace') if trace.exists() else ''
 log=(folder/'copy.log').read_text(encoding='utf-8',errors='replace')
 unchanged=before==(config.read_bytes() if config.exists() else None)
 same_originals=all(hashlib.sha256((folder/name).read_bytes()).hexdigest()==digest for name,digest in originals.items())
 passed=result.returncode==0 and unchanged and same_originals
 if backend==12:passed &= 'Patch4MMS requested=12 actual=12 reason=1' in text
 elif backend==9:
  passed &= 'mmeDeviceOn12=80004002' in log
  if case in ['old-blacklist','optin-disabled']:passed &= 'no enabled mme-d3d9on12 startup request' in log and 'patch4mms: started' not in text
  else:passed &= 'Patch4MMS requested=9 actual=9 reason=2' in text
 else:passed=result.returncode==8 and unchanged and same_originals and 'MMDFacade' in log and 'requires mms.render.mme.on12' in log
 row=dict(case=case,passed=bool(passed),exitCode=result.returncode,configUnchanged=unchanged,originalsUnchanged=same_originals,directory=str(folder))
 rows.append(row);print(json.dumps(row),flush=True);(root/'summary.json').write_text(json.dumps(rows,indent=2))
 if not passed:print(log[-1600:].encode('ascii','backslashreplace').decode());break
print(root/'summary.json');raise SystemExit(0 if all(row['passed'] for row in rows) else 1)
