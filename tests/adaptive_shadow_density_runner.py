"""Verify adaptive allocation when visible triangles have off-screen vertices."""
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
    root = output / str(time.time_ns())
    root.mkdir(parents=True)
    executable = root / source.name
    shutil.copy2(source, executable)
    model = root / 'viewport-covering-triangles.pmx'
    fixture(model, large=True)
    env = os.environ.copy()
    env['LOCALAPPDATA'] = str(root / 'Local')
    env['APPDATA'] = str(root / 'Roaming')
    env['MIKUMIKUSTUDIO_AUTOSAVE_CONFIG'] = str(root / 'autosave.ini')
    results = []
    for mode in (1, 2):
        sizes = []
        for name, x, y, distance in [('centre', 0, 10, 45), ('left', -15, 20, 45),
                                      ('right', 15, 0, 45), ('far', 0, 10, 4800)]:
            stem = root / f'mode-{mode}-{name}'
            command = [str(executable), str(model), '--forbid-mmv', '--throughput', '--processor', 'cpu',
                       '--viewport', '480x360', '--camera', f'{x},{y},0,-{distance},0,0,0',
                       '--self-shadow', str(mode), '--adaptive-shadow', '1', '--fine-shadow', '0',
                       '--shadow-range', '8216', '--warmup', '3', '--samples', '1',
                       '--json', str(stem.with_suffix('.json')), str(stem.with_suffix('.png'))]
            with stem.with_suffix('.log').open('wb') as log:
                run = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW, timeout=40)
            if run.returncode:
                raise RuntimeError(f'{stem.name}: exit {run.returncode}')
            metadata = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8'))
            resolution = metadata['self_shadow']['resolution']
            sizes.append(resolution)
            result = {'case': stem.name, 'resolution': resolution}
            results.append(result)
            print(json.dumps(result), flush=True)
        # The auto-fitted map covers the entire large mesh. At depth 42 its
        # demand exceeds the compensated 4096 ceiling in both modes, even though all
        # vertices are outside the viewport. Only the distant view may shrink.
        expected_near = 4096
        if sizes[:3] != [expected_near] * 3 or sizes[0] <= sizes[3]:
            raise RuntimeError(f'mode {mode}: expected three near maps of {expected_near} and a smaller far map: {sizes}')
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
