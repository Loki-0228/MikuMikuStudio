"""Check real shadow matrices at maximum precision, with optional native MME."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time

from native_shadow_cycle import fixture


def main():
    source, output = map(lambda value: Path(value).resolve(), sys.argv[1:3])
    mme = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
    root = output / str(time.time_ns())
    root.mkdir(parents=True)
    executable = root / ('MikuMikuDance.exe' if mme else source.name)
    shutil.copy2(source, executable)
    if mme:
        for dll in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
            shutil.copy2(mme / dll, root / dll)
    model = root / 'occlusion.pmx'
    fixture(model)
    points = [(-6, 4, 0), (6, 4, 0), (6, 16, 0), (-6, 16, 0),
              (-2, 10, -3), (2, 10, -3), (2, 14, -3), (-2, 14, -3)]
    env = os.environ.copy()
    env['LOCALAPPDATA'] = str(root / 'Local')
    env['APPDATA'] = str(root / 'Roaming')
    env['MIKUMIKUSTUDIO_AUTOSAVE_CONFIG'] = str(root / 'autosave.ini')
    results = []
    for mode in (1, 2):
        references = {}
        for adaptive in (0, 1):
            for name, x, y in [('centre', 0, 10), ('panned', 12, 17)]:
                stem = root / f'mode-{mode}-adaptive-{adaptive}-{name}'
                command = [str(executable), str(model), '--forbid-mmv', '--throughput', '--processor', 'cpu',
                           '--viewport', '480x360', '--camera', f'{x},{y},0,-45,0,0,0',
                           '--self-shadow', str(mode), '--adaptive-shadow', str(adaptive), '--fine-shadow', '0',
                           '--shadow-range', '0', '--warmup', '3', '--samples', '1',
                           '--json', str(stem.with_suffix('.json')), str(stem.with_suffix('.png'))]
                with stem.with_suffix('.log').open('wb') as log:
                    run = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                         creationflags=subprocess.CREATE_NO_WINDOW, timeout=40)
                if run.returncode:
                    raise RuntimeError(f'{stem.name}: exit {run.returncode}')
                metadata = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8'))
                shadow = metadata['self_shadow']
                matrix = [[row[key] for key in ('x', 'y', 'z', 'w')] for row in shadow['light_projection']]
                for point in points:
                    q = [matrix[3][axis] + sum(point[row]*matrix[row][axis] for row in range(3)) for axis in range(4)]
                    if q[3] <= 0:
                        raise RuntimeError(f'{stem.name}: projective pole crosses {point}: {q}')
                    p = [q[axis]/q[3] for axis in range(3)]
                    if abs(p[0]) >= 1 or abs(p[1]) >= 1 or not 0 < p[2] < 1:
                        raise RuntimeError(f'{stem.name}: actual shadow projection clips {point}: {p}')
                    # The native light grid can move with the camera, but a
                    # world-space light ray must keep the same shadow UV.
                    light = [shadow[key] for key in ('light_x', 'light_y', 'light_z')]
                    ray = [point[axis]+.1*light[axis] for axis in range(3)]
                    r = [matrix[3][axis] + sum(ray[row]*matrix[row][axis] for row in range(3)) for axis in range(4)]
                    if max(abs(r[axis]/r[3]-p[axis]) for axis in (0, 1)) > .0001:
                        raise RuntimeError(f'{stem.name}: shadow grid disagrees with light direction')
                if name in references and matrix != references[name]:
                    raise RuntimeError(f'{stem.name}: resolution toggle changed the shadow projection')
                references[name] = matrix
                if shadow['shadow_slider'] != 0 or abs(shadow['distance'] - .1) > 1e-6:
                    raise RuntimeError(f'{stem.name}: auto fit overwrote requested/keyframed range')
                result = {'case': stem.name, 'resolution': shadow['resolution'], 'covered_vertices': len(points)}
                results.append(result)
                print(json.dumps(result), flush=True)
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
