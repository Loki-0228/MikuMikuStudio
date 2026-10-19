"""Hidden production occlusion, orbit stability and adaptive-resolution regression."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time


def text(value):
    data = value.encode('utf-16-le')
    return struct.pack('<i', len(data)) + data


def fixture(path, large=False):
    data = bytearray(b'PMX ' + struct.pack('<f', 2.) + bytes([8, 0, 0, 1, 1, 1, 1, 1, 1]))
    for value in ('shadow-casters-without-outlines', '', '', ''):
        data += text(value)
    data += struct.pack('<i', 8)
    vertices = ((-6, 4, 0), (6, 4, 0), (6, 16, 0), (-6, 16, 0),
                (-2, 10, -3), (2, 10, -3), (2, 14, -3), (-2, 14, -3))
    if large:
        vertices = tuple((x*40, (y-10)*40+10, z) for x, y, z in vertices)
    for x, y, z in vertices:
        data += struct.pack('<8fBBf', x, y, z, 0, 0, -1, 0, 0, 0, 0, 1)
    data += struct.pack('<i', 12) + bytes([0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7])
    data += struct.pack('<ii', 0, 2)
    for name, color in [('receiver', (.6, .6, .6)), ('caster', (.2, .3, .6))]:
        data += text(name) + text(name)
        # PMX flags: double-sided, cast and receive; no outline, edge size zero.
        data += struct.pack('<11fB5fbbBBB', *color, 1, 0, 0, 0, 0, .1, .1, .1,
                            13, 0, 0, 0, 1, 0, -1, -1, 0, 1, 2)
        data += text('') + struct.pack('<i', 6)
    data += struct.pack('<i', 1) + text('center') + text('center')
    data += struct.pack('<3fbiH3f', 0, 0, 0, -1, 0, 26, 0, 0, 0) + struct.pack('<4i', 0, 0, 0, 0)
    path.write_bytes(data)


def main():
    source = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve() / str(time.time_ns())
    root.mkdir(parents=True)
    executable = root / source.name
    shutil.copy2(source, executable)  # Isolate saved UI preferences.
    model = root / 'occlusion.pmx'
    fixture(model)
    results = []
    for mode in (1, 2):
        reference = None
        for yaw in (0, 45, -45):
            name = root / f'mode-{mode}-yaw-{yaw}'
            command = [str(executable), str(model), '--forbid-mmv', '--throughput', '--processor', 'cpu',
                       '--viewport', '480x360', '--camera', f'0,10,0,-45,{yaw},0,0',
                       '--self-shadow', str(mode), '--adaptive-shadow', '0', '--fine-shadow', '0',
                       '--shadow-range', '8216', '--warmup', '10', '--samples', '2',
                       '--verify-shadow-cycle', '--json', str(name.with_suffix('.json')), str(name.with_suffix('.png'))]
            env = os.environ.copy()
            env['LOCALAPPDATA'] = str(root / 'Local')
            env['APPDATA'] = str(root / 'Roaming')
            env['MIKUMIKUSTUDIO_AUTOSAVE_CONFIG'] = str(root / 'autosave.ini')
            run = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW, timeout=40)
            name.with_suffix('.log').write_bytes(run.stdout)
            output = run.stdout.decode(errors='replace')
            if run.returncode:
                raise RuntimeError(f'{name.name}: exit {run.returncode}\n{output}')
            widths = re.search(r'dynamic_width=(\d+) wide_width=(\d+)', output)
            if not widths or int(widths[1]) > int(widths[2]):
                raise RuntimeError(f'{name.name}: wider coverage lost shadow sampling density')
            digest = hashlib.sha256(Path(str(name) + '.png.fixed.png.shadow.dds').read_bytes()).hexdigest()
            if reference is not None and digest != reference:
                raise RuntimeError(f'{name.name}: camera orbit changed the directional shadow map')
            reference = digest
            result = {'case': name.name, 'map_sha256': digest, 'dynamic_width': int(widths[1]), 'wide_width': int(widths[2])}
            results.append(result)
            print(json.dumps(result), flush=True)
    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
