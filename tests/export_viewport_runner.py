"""Isolated AVI completion/cancellation and window-DPI regression scenarios.

Optional real MME coverage:
  --mme-dir <folder containing the three MME DLLs>
  --effect <effect accessory .x> (repeatable)
Requires Windows 10+ and a D3D9 device. High-DPI screens exercise the actual
coordinate mismatch; context/geometry assertions also run at 100% scaling.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--mme-dir', type=Path)
parser.add_argument('--effect', type=Path, action='append', default=[])
parser.add_argument('--case', action='append', choices=['ordinary', 'dpi-change', 'aware-dpi-change', 'scaled-dpi-change', 'reset-failure', 'camera-modes'])
parser.add_argument('--aa', choices=['fxaa', 'taa'])
parser.add_argument('--scale', type=int)
args = parser.parse_args()
executable = args.executable.resolve()
effects = [str(path.absolute()) for path in args.effect]
for path in effects:
    if not Path(path).is_file():
        parser.error(f'Effect accessory does not exist: {path}')
if effects and not args.mme_dir:
    parser.error('--effect requires --mme-dir')
root = args.output.resolve() / str(time.time_ns())
root.mkdir(parents=True)
fixture = '''xof 0303txt 0032
Mesh Cube {
8;
-10;0;-10;,10;0;-10;,10;20;-10;,-10;20;-10;,
-10;0;10;,10;0;10;,10;20;10;,-10;20;10;;
12;
3;0,2,1;,3;0,3,2;,3;4,5,6;,3;4,6,7;,
3;0,1,5;,3;0,5,4;,3;3,7,6;,3;3,6,2;,
3;0,4,7;,3;0,7,3;,3;1,2,6;,3;1,6,5;;
MeshMaterialList {
1;12;0,0,0,0,0,0,0,0,0,0,0,0;;
Material {0.2;0.5;0.9;1.0;;5.0;0.1;0.1;0.1;;0.1;0.1;0.1;;}
}
}
'''
cases = [
    ('ordinary', 'unaware', {}),
    ('dpi-change', 'unaware', {'PROBE_DPI_CHANGE': '1'}),
    ('aware-dpi-change', 'aware', {'PROBE_DPI_CHANGE': '1'}),
    ('scaled-dpi-change', 'unaware', {'PROBE_DPI_CHANGE': '1', 'PROBE_SCALE': '50'}),
    ('reset-failure', 'unaware', {'PROBE_RESET_BLOCKER': '1'}),
    ('camera-modes', 'unaware', {'PROBE_CAMERA_MODES': '1'}),
]
results = []
for name, dpi, overrides in cases:
    if args.case and name not in args.case:
        continue
    directory = root / name
    (directory / 'Data').mkdir(parents=True)
    copy = directory / 'MikuMikuDance.exe'
    shutil.copy2(executable, copy)
    if args.mme_dir:
        for dll in ('MMEffect.dll', 'MMHack.dll', 'd3d9.dll'):
            shutil.copy2(args.mme_dir / dll, directory / dll)
    (directory / 'Data/mmconfig.ini').write_text('40\n40\n1200\n850\n0\n1920\n1080\n250\n2\n')
    (directory / 'fixture.x').write_text(fixture)
    if name == 'camera-modes':
        from pmd_selection_fixtures import make_fixture
        make_fixture(directory / 'model', 255, [''] * 10)
    env = {key: value for key, value in os.environ.items() if not key.startswith('PROBE_')}
    env.update(LOCALAPPDATA=str(directory / 'Local'), APPDATA=str(directory / 'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(directory / 'autosave.ini'),
               MMS_ANTIALIAS_CONFIG=str(directory / 'antialias.ini'), **overrides)
    if args.mme_dir:
        env['PROBE_REQUIRE_MME'] = '1'
    if args.aa:
        env['PROBE_AA'] = args.aa
    if args.scale:
        env['PROBE_SCALE'] = str(args.scale)
    command = [str(copy), dpi, 'normal', '1280', '720', *effects]
    with (directory / 'result.log').open('wb') as log:
        try:
            run = subprocess.run(command, cwd=directory, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=60,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            code = run.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    result = {'case': name, 'exit_code': code, 'log': str(directory / 'result.log')}
    results.append(result)
    print(json.dumps(result), flush=True)
(root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
raise SystemExit(any(result['exit_code'] != 0 for result in results))
