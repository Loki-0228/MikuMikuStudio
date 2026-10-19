"""Compare mixed native/MME projected shadows in an isolated hidden host."""
from pathlib import Path
import argparse
import json
import os
import shutil
import struct
import subprocess
import time
from collections import Counter
from PIL import Image
import numpy as np
from mme_display_fixtures import text


def fixture(path):
    data = bytearray(b'PMX ' + struct.pack('<f', 2.) + bytes([8, 0, 0, 1, 1, 1, 1, 1, 1]))
    for value in ('mixed-ground-shadow', '', '', ''):
        data += text(value)
    data += struct.pack('<i', 8)
    for x, y in ((-8, 4), (-1, 4), (-1, 12), (-8, 12), (1, 4), (8, 4), (8, 12), (1, 12)):
        data += struct.pack('<8fBBf', x, y, 0, 0, 0, -1, 0, 0, 0, 0, 1)
    data += struct.pack('<i', 12) + bytes([0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7])
    data += struct.pack('<ii', 0, 2)
    for name in ('native', 'mme'):
        data += text(name) + text(name)
        data += struct.pack('<11fB5fbbBBB', .7, .2, .1, 1, 0, 0, 0, 0, .1, .1, .1,
                            15, 0, 0, 0, 1, 1, -1, -1, 0, 1, 0)
        data += text('') + struct.pack('<i', 6)
    data += struct.pack('<i', 1) + text('center') + text('center')
    data += struct.pack('<3fbiH3f', 0, 0, 0, -1, 0, 26, 0, 0, 0) + struct.pack('<4i', 0, 0, 0, 0)
    path.write_bytes(data)
    path.with_suffix('.emd').write_text('[Info]\nVersion = 3\n[Effect]\nObj = none\nObj[1] = shadow.fx\n', encoding='ascii')
    (path.parent / 'shadow.fx').write_text('''
float4 EgColor;
float4x4 WorldViewProjection : WORLDVIEWPROJECTION;
float4 VS(float4 p : POSITION) : POSITION { return mul(p, WorldViewProjection); }
float4 PS() : COLOR0 { return float4(EgColor.rgb, 0.65); }
technique Shadow < string MMDPass = "shadow"; > {
    pass P { VertexShader = compile vs_2_0 VS(); PixelShader = compile ps_2_0 PS(); }
}
technique Object < string MMDPass = "object"; > { }
technique ObjectSS < string MMDPass = "object_ss"; > { }
technique Edge < string MMDPass = "edge"; > { }
technique Zplot < string MMDPass = "zplot"; > { }
''', encoding='ascii')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('mme', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--model', type=Path)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    root = args.output.resolve() / str(time.time_ns())
    root.mkdir(parents=True)
    host = root / 'MikuMikuDance.exe'
    shutil.copy2(args.executable, host)
    for name in (() if args.native else ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll')):
        shutil.copy2(args.mme / name, root / name)
    (root / 'Data').mkdir()
    (root / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
    model = args.model.resolve() if args.model else root / 'mixed.pmx'
    if not args.model:
        fixture(model)
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root/'Local'), APPDATA=str(root/'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(root/'autosave.ini'))
    if args.native:
        env['MMS_MME_ORDER_NATIVE'] = '1'
    with (root/'result.log').open('wb') as log:
        result = subprocess.run([str(host), str(model), '--ground-shadow', str(root)],
            cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=120)
    print(root, flush=True)
    if result.returncode:
        print((root/'result.log').read_text(encoding='utf-8', errors='replace'))
        return result.returncode
    checks = []
    for path in sorted(root.glob('*-on.ppm')):
        on = Image.open(path).convert('RGB')
        off = Image.open(path.with_name(path.name.replace('-on.ppm', '-off.ppm'))).convert('RGB')
        pixels = np.asarray(on)
        background = np.asarray(off)
        mask = np.all(background == 255, axis=2) & np.any(pixels != background, axis=2)
        # Exclude MSAA silhouette boundaries, including partially covered pixels.
        for axis in (0, 1):
            for step in (-2, -1, 1, 2):
                mask &= np.all(pixels == np.roll(pixels, step, axis=axis), axis=2)
        mask[:2] = mask[-2:] = False
        mask[:, :2] = mask[:, -2:] = False
        colors = Counter(map(tuple, pixels[mask].tolist()))
        # Ignore anti-aliased boundary shades; both large silhouettes must have
        # the same interior shade regardless of the material's MME assignment.
        dominant = [(rgb, count) for rgb, count in colors.most_common() if count > 100]
        spread = max((max(rgb) for rgb, _ in dominant), default=0) - min((min(rgb) for rgb, _ in dominant), default=255)
        check = {'case': path.stem, 'colors': dominant, 'spread': spread,
                 'passed': bool(dominant) and spread <= 2}
        checks.append(check)
        on.save(path.with_suffix('.png'))
        print(json.dumps(check), flush=True)
    (root/'results.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    return int(not checks or any(not check['passed'] for check in checks))


if __name__ == '__main__':
    raise SystemExit(main())
