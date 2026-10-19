"""Measure finest shadow density and actual resource resizing in one process."""
from pathlib import Path
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import time

from native_shadow_cycle import fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--mme', type=Path)
    parser.add_argument('--baseline', action='store_true')
    options = parser.parse_args()
    root = options.output.resolve() / str(time.time_ns())
    root.mkdir(parents=True)
    executable = root / ('MikuMikuDance.exe' if options.mme else 'frame_benchmark.exe')
    shutil.copy2(options.executable, executable)
    if options.mme:
        for dll in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
            shutil.copy2(options.mme / dll, root / dll)
    model = root / 'occlusion.pmx'
    fixture(model)
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root / 'Local'), APPDATA=str(root / 'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(root / 'autosave.ini'))
    results, zoom = [], []
    for mode in (1, 2):
        for adaptive in (0, 1):
            stem = root / f'mode-{mode}-adaptive-{adaptive}'
            command = [str(executable), str(model), '--forbid-mmv', '--throughput', '--processor', 'cpu',
                       '--viewport', '960x720', '--camera', '0,10,0,-45,0,0,0',
                       '--self-shadow', str(mode), '--adaptive-shadow', str(adaptive), '--fine-shadow', '1',
                       '--shadow-range', '0', '--warmup', '3', '--samples', '1',
                       '--json', str(stem.with_suffix('.json')), str(stem.with_suffix('.png'))]
            if not options.baseline and adaptive:
                command.append('--verify-shadow-zoom')
            with stem.with_suffix('.log').open('wb') as log:
                run = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW, timeout=100)
            if run.returncode:
                raise RuntimeError(f'{stem.name}: exit {run.returncode}; see {stem.with_suffix(".log")}')
            shadow = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8'))['self_shadow']
            matrix = shadow['light_projection']
            scale = math.sqrt(sum(matrix[row]['x']**2 for row in range(3)))
            result = dict(case=stem.name, resolution=shadow['resolution'], coverage_width=2/scale,
                          texels_per_world_unit=shadow['resolution']*scale/2,
                          previous_finest_density=4096 * (.2 if mode == 1 else .3) / 2)
            if not options.baseline:
                assert shadow['surface_width'] == shadow['resolution'] == shadow['surface_height']
                if not adaptive:
                    # The explicit 8192 resource ceiling may limit compensation
                    # for mode 2 or a large scene. It must still double this
                    # fixture's density compared with the pre-compensation map.
                    assert shadow['resolution'] == 8192
                assert shadow['shadow_slider'] == 0 and abs(shadow['distance']-.1) < 1e-6
            results.append(result)
            print(json.dumps(result), flush=True)
            output = stem.with_suffix('.log').read_text(encoding='utf-8', errors='replace')
            for enabled, distance, surface, texture, coverage in re.findall(
                    r'shadow_zoom adaptive=(\d+) distance=(\d+) surface=(\d+) texture=(\d+) coverage_preserved=(\d+)', output):
                row = dict(mode=mode, adaptive=int(enabled), distance=int(distance),
                           surface=int(surface), texture=int(texture), coverage_preserved=int(coverage))
                zoom.append(row)
                print(json.dumps(row), flush=True)
    (root / 'results.json').write_text(json.dumps(dict(finest=results, zoom=zoom), indent=2), encoding='utf-8')
    print(f'PASS {root}', flush=True)


if __name__ == '__main__':
    main()
