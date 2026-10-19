"""Exercise inherited shadow sampler states through real MME in a hidden host."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import time
from native_shadow_cycle import fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('mme', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--model', type=Path)
    args = parser.parse_args()
    root = args.output.absolute() / str(time.time_ns())
    root.mkdir(parents=True)
    host = root / 'MikuMikuDance.exe'
    shutil.copy2(args.executable, host)
    for name in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
        shutil.copy2(args.mme / name, root / name)
    (root / 'Data').mkdir()
    (root / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
    model = args.model.absolute() if args.model else root / 'receiver.pmx'
    if not args.model:
        fixture(model)
        model.with_suffix('.emd').write_text(
            '[Info]\nVersion = 3\n[Effect]\nObj = none\nObj[0] = receiver.fx\nObj[1] = none\n',
            encoding='ascii')
        (root / 'receiver.fx').write_text('''
float4x4 WorldViewProjection : WORLDVIEWPROJECTION;
// Deliberately inherit host sampler states, like AlternativeFull.
sampler DefSampler : register(s0);
struct Output { float4 position : POSITION; float2 uv : TEXCOORD0; };
Output VS(float4 position : POSITION) {
    Output result;
    result.position = mul(position, WorldViewProjection);
    result.uv = position.xy * .03 + .2;
    return result;
}
float4 PS(Output input) : COLOR0 { return float4(tex2D(DefSampler, input.uv).rrr, 1); }
technique Main < string MMDPass = "object_ss"; > {
    pass P { CullMode = None;
        VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); }
}
''', encoding='ascii')
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root/'Local'), APPDATA=str(root/'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(root/'autosave.ini'))
    with (root / 'result.log').open('wb') as log:
        result = subprocess.run([str(host), str(model), str(root)], cwd=root, env=env,
            stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW, timeout=90)
    print(root, flush=True)
    print((root/'result.log').read_text(encoding='utf-8', errors='replace'), flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
