"""Opt-in real DLL, product window and D3D9 tests; all inputs are copied."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from run_mmdfacade_candidates import digest, observer_manifest, write_json
from run_legacy_mme_probe import isolated_environment

def main():
    parser=argparse.ArgumentParser()
    for key in ['build','effekseer','effect','mme']:
        parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--single',action='store_true')
    parser.add_argument('--mme-off',action='store_true')
    parser.add_argument('--ascii-effect',action='store_true')
    parser.add_argument('--no-mme',action='store_true')
    args=parser.parse_args()
    for key in ['build','effekseer','effect','mme']: setattr(args,key,getattr(args,key).resolve())
    root=Path(tempfile.mkdtemp(prefix='mmdfacade-functional-',dir=args.build.parent))
    cases=[]
    for layout in ['legacy-mmdplugin','PLUGINS/legacy-mmdplugin']:
        for name in ['MikuMikuDance.exe','Renamed Studio.exe','中文工作室.exe']:
            directory=root/str(len(cases)); directory.mkdir()
            exe=directory/name
            shutil.copy2(args.build/'tests/mme/Release/MikuMikuDance.exe',exe)
            for dll in ['MMDFacade.dll','MMHack.dll']: shutil.copy2(args.build/'Release'/dll,directory/dll)
            provider=directory/layout/'MMDPlugin.dll'; provider.parent.mkdir(parents=True)
            shutil.copy2(args.build/'Release/legacy-mmdplugin/MMDPlugin.dll',provider)
            for source,profile,identity in [
                (args.effekseer,'effekseer_v2','test.effekseer'),
                (args.build/'Release/optional-plugins/first-person-camera/MMSFirstPersonCamera.dll','first_person_camera_v4','test.camera')]:
                package=directory/'PLUGINS'/identity
                shutil.copytree(source.parent,package)
                write_json(package/'plugin.json',observer_manifest(source,identity,profile))
            mme=directory/'PLUGINS/mme'; mme.mkdir()
            shutil.copy2(args.mme/'MMEffect.dll',mme/'MMEffect.dll')
            (mme/'MMEffect.ini').write_text('[System]\nSkipValidation=true\nEMMAutoSave=false\n',encoding='ascii')
            write_json(mme/'plugin.json',dict(schemaVersion=1,id='test.mme',name='MME',version='0.37.0',kind='legacy_mme',entry='MMEffect.dll',architecture='x64',minWindows='6.1.0',reloadPolicy='restart'))
            if args.no_mme: (mme/'plugin.json').unlink()
            data=directory/'data'; data.mkdir()
            write_json(data/'settings.json',dict(schemaVersion=1,sections=dict(processor=dict(vertices=dict(mode='cpu')))))
            scene=directory/('effect' if args.ascii_effect else '中文特效'); shutil.copytree(args.effect.parent,scene)
            effect=scene/args.effect.name
            env=isolated_environment(directory); env['MIKUMIKUSTUDIO_TRACE_FILE']=str(directory/'runtime.log')
            if args.mme_off: env['MMDFACADE_TEST_MME_OFF']='1'
            if args.no_mme: env['MMDFACADE_TEST_NO_MME']='1'
            case=dict(exe=name,layout=layout,directory=str(directory),passed=False)
            with (directory/'result.log').open('wb') as output:
                try:
                    run=subprocess.run([str(exe),str(effect),'1','--facade-real'],cwd=directory,env=env,stdout=output,stderr=subprocess.STDOUT,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
                    case['exitCode']=run.returncode
                    case['passed']=run.returncode==0
                except subprocess.TimeoutExpired: case['error']='timeout'
            case['sourcesUnchanged']=digest(directory/'PLUGINS/test.effekseer'/args.effekseer.name)==digest(args.effekseer)
            case['passed'] &= case['sourcesUnchanged']
            cases.append(case)
            write_json(root/'summary.json',dict(passed=all(c['passed'] for c in cases) and (args.single or len(cases)==6),cases=cases))
            print(json.dumps(case,ensure_ascii=True),flush=True)
            if not case['passed'] or args.single:
                print(root/'summary.json'); return 0 if case['passed'] else 1
    print(root/'summary.json'); return 0
if __name__=='__main__': raise SystemExit(main())
