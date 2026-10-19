"""Create original fixtures and exercise same-name EMD loading in hidden native MME."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time

from mme_display_fixtures import fixture


def main():
    executable, mme, output = map(lambda value: Path(value).resolve(), sys.argv[1:4])
    file_watch = '--file-watch' in sys.argv[4:]
    run = output / str(time.time_ns())
    results = []
    effect = '''float4x4 WorldViewProjection : WORLDVIEWPROJECTION;
float4 VS(float4 p : POSITION) : POSITION { return mul(p, WorldViewProjection); }
float4 PS() : COLOR0 { return float4(1, 0, 1, 1); }
technique Main < string MMDPass = "object"; > {
    pass P { CullMode = None; AlphaBlendEnable = false;
        VertexShader = compile vs_2_0 VS(); PixelShader = compile ps_2_0 PS(); }
}
'''
    for name, relative in [('ascii', 'models'), ('cp932-chinese', 'Digitrvx模'),
                           ('chinese', '中文渲染目录'), ('japanese', 'モデル髪')]:
        directory = run / name
        directory.mkdir(parents=True)
        shutil.copy2(executable, directory / 'MikuMikuDance.exe')
        for dll in ['d3d9.dll', 'MMEffect.dll', 'MMHack.dll']:
            shutil.copy2(mme / dll, directory / dll)
        (directory / 'Data').mkdir()
        (directory / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
        models = directory / relative
        (models / 'fx').mkdir(parents=True)
        model = models / 'model.pmx'
        model.write_bytes(fixture())
        model.with_suffix('.emd').write_text('[Info]\nVersion = 3\n\n[Effect]\nObj = fx\\assigned.fx\nObj.show = true\n', encoding='ascii')
        (models / 'fx/assigned.fx').write_text(effect, encoding='ascii')
        env = os.environ.copy()
        env['LOCALAPPDATA'] = str(directory / 'Local')
        env['APPDATA'] = str(directory / 'Roaming')
        env['MIKUMIKUSTUDIO_AUTOSAVE_CONFIG'] = str(directory / 'autosave.ini')
        with (directory / 'result.log').open('wb') as log:
            arguments = ['--file-watch', str(models / 'fx/assigned.fx')] if file_watch else ['--emd-autoload']
            result = subprocess.run([str(directory / 'MikuMikuDance.exe'), str(model), *arguments],
                                    cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=40)
        summary = {'case': name, 'exit_code': result.returncode, 'log': str(directory / 'result.log')}
        results.append(summary)
        print(json.dumps(summary), flush=True)
        print((directory / 'result.log').read_text(encoding='utf-8', errors='replace'), flush=True)
    (run / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return int(any(result['exit_code'] != 0 for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
