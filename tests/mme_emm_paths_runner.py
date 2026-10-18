"""Exercise existing absolute and relative EMM paths through native MME."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

from mme_display_fixtures import fixture


EFFECT = '''float4x4 WorldViewProjection : WORLDVIEWPROJECTION;
float4 VS(float4 p : POSITION) : POSITION { return mul(p, WorldViewProjection); }
float4 PS() : COLOR0 { return float4(1, 0, 1, 1); }
technique Main < string MMDPass = "object"; > {
    pass P { CullMode = None; AlphaBlendEnable = false;
        VertexShader = compile vs_2_0 VS(); PixelShader = compile ps_2_0 PS(); }
}
'''


def main():
    executable, mme, output = (Path(value).resolve() for value in sys.argv[1:4])
    run = output / str(time.time_ns())
    cases = [
        ('absolute-ascii', 'effects', 'ascii', False),
        ('absolute-spaces', 'effect files', 'ascii', False),
        ('relative-spaces', 'effect files', 'ascii', True),
        ('absolute-cp932', 'エフェクト', 'cp932', False),
        ('relative-cp932', 'エフェクト', 'cp932', True),
        ('absolute-dbcs-separator', 'ソフト', 'cp932', False),
        ('absolute-cp936', '中文效果', 'cp936', False),
        ('absolute-utf8', '中文效果', 'utf-8', False),
        ('missing-absolute', 'エフェクト', 'cp932', False),
        ('exe-relative-backup', 'effects', 'ascii', True),
        ('exe-relative-backup-cp936', '中文效果', 'cp936', True),
        ('exe-relative-parent', 'effects', 'ascii', True),
        ('exe-relative-other-cwd', 'effects', 'ascii', True),
        ('project-relative-other-cwd', 'effects', 'ascii', True),
        ('project-relative-priority', 'effect files', 'ascii', True),
        ('project-relative-cp936', '中文效果', 'cp936', True),
        ('project-relative-utf8', '中文效果', 'utf-8', True),
        ('project-relative-cp932', 'ソフト', 'cp932', True),
    ]
    selected = set(sys.argv[4:])
    results = []
    for name, folder, encoding, relative in cases:
        if selected and name not in selected:
            continue
        directory = run / name
        directory.mkdir(parents=True)
        shutil.copy2(executable, directory / 'MikuMikuDance.exe')
        for dll in ['d3d9.dll', 'MMEffect.dll', 'MMHack.dll']:
            shutil.copy2(mme / dll, directory / dll)
        (directory / 'Data').mkdir()
        (directory / 'Data/mmconfig.ini').write_text(
            '40\n40\n1000\n740\n0\n640\n480\n250\n1\n', encoding='ascii')
        model = directory / 'model.pmx'
        model.write_bytes(fixture())
        project_base = directory / 'projects' if name.startswith('project-relative') else directory
        project_base.mkdir(exist_ok=True)
        effects = project_base / folder
        effects.mkdir()
        effect = effects / 'assigned.fx'
        effect.write_text(EFFECT, encoding='ascii')
        if name == 'project-relative-priority':
            legacy = directory / folder
            legacy.mkdir()
            (legacy / 'assigned.fx').write_text(EFFECT.replace('float4(1, 0, 1, 1)', 'float4(0, 1, 0, 1)'), encoding='ascii')
        project = project_base / 'scene.pmm'
        if name.startswith('exe-relative'):
            project = directory / ('data/scene.pmm' if name == 'exe-relative-parent' else 'data/backups/scene.pmm')
            project.parent.mkdir(parents=True, exist_ok=True)
        project.write_bytes(b'EMM notification fixture\n')
        assignment = project.with_suffix('.emm')
        effect_path = str(effect.relative_to(project_base) if relative else effect)
        if name == 'exe-relative-parent':
            # A project-relative spelling written before the backup was moved.
            effect_path = '..\\' + effect_path
        text = (f'[Info]\nVersion = 3\n\n[Object]\nPmd1 = {model}\n\n'
                f'[Effect]\nDefault = none\nPmd1 = {effect_path}\nPmd1.show = true\n')
        assignment.write_bytes(text.replace('\n', '\r\n').encode(encoding))
        assert effect.is_file() and model.is_file()
        missing = name == 'missing-absolute'
        if missing:
            effect.unlink()
        before = hashlib.sha256(assignment.read_bytes()).hexdigest()
        env = os.environ.copy()
        env['LOCALAPPDATA'] = str(directory / 'Local')
        env['APPDATA'] = str(directory / 'Roaming')
        env['MIKUDANCESTUDIO_AUTOSAVE_CONFIG'] = str(directory / 'autosave.ini')
        if name.endswith('other-cwd'):
            (directory / 'browse').mkdir()
            env['MMS_EMM_WORKING_DIRECTORY'] = str(directory / 'browse')
        with (directory / 'result.log').open('wb') as log:
            try:
                completed = subprocess.run(
                    [str(directory / 'MikuMikuDance.exe'), str(model),
                     '--emm-load', str(project)], cwd=directory, env=env,
                    stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=40)
                code = completed.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        unchanged = hashlib.sha256(assignment.read_bytes()).hexdigest() == before
        log_text = (directory / 'result.log').read_text(encoding='utf-8', errors='replace')
        passed = (code == 2 and 'The following effect files do not exist:' in log_text) if missing else code == 0
        result = {'case': name, 'exit_code': code, 'passed': passed, 'emm_unchanged': unchanged,
                  'log': str(directory / 'result.log')}
        results.append(result)
        print(json.dumps(result), flush=True)
        print(log_text, flush=True)
    (run / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return int(any(not item['passed'] or not item['emm_unchanged'] for item in results))


if __name__ == '__main__':
    raise SystemExit(main())
