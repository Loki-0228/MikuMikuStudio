"""Exercise the shipped enable/disable entry points without modifying an installation."""
import argparse,json,os,shutil,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(tempfile.mkdtemp(prefix='patch-config-',dir=a.output.resolve().parent))
package=root/'PLUGINS/中文 分组/补丁';shutil.copytree(a.package,package)
config=root/'PLUGINS/config.json'
original=dict(schemaVersion=1,maxDepth=4,disabledPlugins=['other.blocked','patch4mms'],enabledPlugins=['other.enabled'],custom=dict(retain=17))
config.write_text(json.dumps(original),encoding='utf-8'); rows=[]
def run(action):
 env=os.environ.copy();env['MMS_PATCH_NO_PAUSE']='1'
 result=subprocess.run([os.environ['COMSPEC'],'/d','/c',str(package/(action+'.cmd'))],cwd=package,env=env,
    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
 (root/(action+str(len(rows))+'.log')).write_bytes(result.stdout)
 return result.returncode
for action,enabled in [('Enable',True),('Enable',True),('Disable',False),('Enable',True)]:
 code=run(action); value=json.loads(config.read_text(encoding='utf-8-sig'))
 passed=code==0 and value['maxDepth']==4 and value['custom']==original['custom'] and 'other.blocked' in value['disabledPlugins'] and 'other.enabled' in value['enabledPlugins']
 passed &= value['enabledPlugins'].count('patch4mms')==(1 if enabled else 0)
 passed &= ('patch4mms' in value['disabledPlugins'])!=enabled
 rows.append(dict(action=action,passed=passed,exitCode=code))
config.write_bytes(b'{ invalid config'); before=config.read_bytes();code=run('Enable')
rows.append(dict(action='preserve-invalid-config',passed=code!=0 and config.read_bytes()==before,exitCode=code))
a.output.write_text(json.dumps(dict(directory=str(root),passed=all(row['passed'] for row in rows),cases=rows),indent=2))
print(a.output);raise SystemExit(0 if all(row['passed'] for row in rows) else 1)
