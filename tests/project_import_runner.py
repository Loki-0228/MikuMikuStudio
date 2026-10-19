"""Run generated PMM import regressions in isolated, hidden production hosts."""
from pathlib import Path
import argparse
import json
import os
import shutil
import struct
import subprocess
import time

from mme_display_fixtures import fixture


def patched(data, offset, value, fmt='<I'):
    result = bytearray(data)
    struct.pack_into(fmt, result, offset, value)
    return result


def model_offsets(data):
    """Locate fields in the original one-model PMM written by the host."""
    cursor = 56  # common header, selected slot, count, first model slot

    def advance(size):
        nonlocal cursor
        start = cursor
        cursor += size
        return start

    def count():
        return struct.unpack_from('<i', data, advance(4))[0]

    def name():
        advance(data[advance(1)])

    name(); name()
    path = advance(256)
    groups = data[advance(1)]
    counts = []
    for index in range(4):
        counts.append(cursor)
        length = count()
        for _ in range(length):
            name() if index < 2 else advance(4)
    bones, morphs, iks, parents = [struct.unpack_from('<i', data, at)[0] for at in counts]
    state = advance(2)  # order, visible
    selected_bone = advance(4)
    advance(16 + 1 + groups + 8)
    bone_keys = advance(58 * bones)
    advance(count() * 62)
    advance(17 * morphs)
    advance(count() * 21)
    display_size = 14 + iks + parents * 8
    display = advance(display_size)
    advance(count() * (display_size + 4))
    pose = advance(31 * bones + 4 * morphs + iks + 16 * parents + 7)
    return dict(path=path, count=counts[0], bone=bone_keys, selected=selected_bone,
                state=state, pose=pose, display=display, end=cursor)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--mme', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', help='Run only these fixture names')
    args = parser.parse_args()
    folder = args.output.resolve() / str(time.time_ns())
    folder.mkdir(parents=True)
    exe = folder / 'MikuMikuDance.exe'
    shutil.copy2(args.host.resolve(), exe)
    for name in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
        shutil.copy2(args.mme.resolve() / name, folder / name)
    (folder / 'data').mkdir()
    (folder / 'data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
    model = folder / 'model.pmx'
    model.write_bytes(fixture())
    (folder / 'fixture.x').write_text('''xof 0303txt 0032
Mesh Triangle {3;-2;10;0;,2;10;0;,0;14;0;;1;3;0,1,2;;
MeshMaterialList {1;1;0;;Material {0.2;0.5;0.9;1.0;;5;0.1;0.1;0.1;;0;0;0;;}}
}
''', encoding='ascii')
    env = dict(os.environ, MMS_ANTIALIAS_CONFIG=str(folder / 'aa.ini'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(folder / 'autosave.ini'), MMS_NVIDIA_AA_FIX='0',
               LOCALAPPDATA=str(folder / 'Local'), APPDATA=str(folder / 'Roaming'))
    results = []

    def run(label, command, warnings=0):
        log_path = folder / (label + '.log')
        with log_path.open('wb') as log:
            try:
                child = subprocess.run([str(exe), *map(str, command)], cwd=folder,
                                       env=dict(env, MMS_IMPORT_WARNINGS=str(warnings)),
                                       stdout=log, stderr=subprocess.STDOUT, timeout=35,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                code = child.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        result = dict(case=label, exit=code, log=str(log_path))
        print(json.dumps(result), flush=True)
        results.append(result)
        (folder / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        return code == 0

    if not run('template', [model, '--import-template', folder]):
        return 1
    empty = (folder / 'empty.pmm').read_bytes()
    original = (folder / 'model.pmm').read_bytes()
    accessory = (folder / 'accessory.pmm').read_bytes()
    # The shadow-list label precedes a 452-byte accessory block (one key).
    acc_start, acc_key, acc_end = 345, 703, 797
    assert accessory[244] == 1 and accessory[acc_start] == 0
    fields = model_offsets(original)
    assert original[fields['end']:fields['end'] + 78] == empty[55:133], 'fixture parser reaches the camera section'
    # The earliest V1 file has the same mandatory config through capture mode,
    # an eight-byte shorter camera key, and no optional extension tail.
    v1 = bytearray(empty[:52] + empty[53:95] + empty[103:1082])
    v1[20:25] = b'0001\0'
    # A genuine V1 model block has fixed names, no remap tables, no parent
    # payloads, and repeats quaternion X in its current-pose record.
    label = b'Display Model\0'.ljust(20, b'\0')
    model_v1 = bytearray(original[:52] + original[53:55] + label + b'\0' + label)
    model_v1[20:25] = b'0001\0'
    model_v1 += original[fields['path']:fields['path'] + 256]
    model_v1 += original[fields['state']:fields['bone']]
    model_v1 += original[fields['bone']:fields['bone'] + 57] + struct.pack('<ii', 0, 0)
    model_v1 += original[fields['display']:fields['display'] + 13] + b'\0' + struct.pack('<i', 0)
    pose = original[fields['pose']:fields['pose'] + 31]
    model_v1 += pose[:28] + pose[12:16] + pose[29:31]
    camera = fields['end']
    model_v1 += original[camera:camera + 40] + original[camera + 48:camera + (1082 - 55)]
    cases = [
        ('empty-v2', empty, 0), ('model-v2', original, 0), ('empty-v1', v1, 0),
        ('model-v1', model_v1, 0), ('model-before-render', (folder / 'model-cold.pmm').read_bytes(), 0),
        ('empty-v1-optional', v1 + empty[1082:1090], 0),
        ('empty-v3', patched(empty, 23, ord('3'), '<B'), 0),
        ('selected-255', patched(patched(empty, 46, 0, '<B'), 53, 255, '<B'), 0),
        ('v1-selected-255', patched(patched(v1, 46, 0, '<B'), 52, 255, '<B'), 0),
        ('v1-model-count-without-records', patched(v1, 53, 255, '<B'), 1),
        ('v1-truncated-model-mode', patched(patched(v1, 46, 0, '<B'), 53, 255, '<B'), 1),
        ('model-selected-missing', patched(original, 53, 254, '<B'), 0),
        ('model-slot-oob', patched(original, 55, 255, '<B'), 0),
        ('camera-link-oob', patched(empty, 63, 0xffffffff), 1),
        ('camera-link-empty', patched(empty, 63, 1), 1),
        ('light-link-oob', patched(empty, 182, 0xffffffff), 1),
        ('shadow-link-oob', patched(empty, 1164, 0xffffffff), 1),
        ('gravity-link-oob', patched(empty, 1121, 0xffffffff), 1),
        ('negative-count', patched(empty, 133, -1, '<i'), 1),
        ('oversized-count', patched(empty, 133, 0x7fffffff), 1),
        ('oversized-bone-count', patched(original, fields['count'], 0x7fffffff), 1),
        ('bone-link-oob', patched(original, fields['bone'] + 8, 0xffffffff), 1),
        ('display-link-oob', patched(original, fields['display'] + 8, 0xffffffff), 1),
        ('selected-bone-oob', patched(original, fields['selected'], 0x7fffffff), 0),
        ('camera-parent-empty', patched(empty, 1191, 17), 0),
        ('camera-parent-oob', patched(empty, 1191, 0x7fffffff), 0),
        ('camera-key-parent-empty', patched(empty, 95, 17), 0),
        ('camera-key-parent-oob', patched(empty, 95, 0x7fffffff), 0),
        ('short-v2-header', empty[:10], 1), ('short-v2-camera', empty[:90], 1),
        ('short-v2-config', empty[:600], 1), ('short-v2-tail', empty[:1193], 1),
        ('short-v1-camera', v1[:90], 1), ('short-v1-config', v1[:600], 1),
        ('short-v1-optional', v1 + b'\x01', 1),
        ('unknown-version', patched(empty, 23, ord('9'), '<B'), 1),
        ('model-path-unterminated', original[:fields['path']] + b'A' * 256 + original[fields['path'] + 256:], 1),
        ('model-missing', original[:fields['path']] + b'missing.pmx\0'.ljust(256, b'\0') + original[fields['path'] + 256:], 1),
        ('model-duplicate-slot', patched(original[:fields['end']] + original[55:fields['end']] + original[fields['end']:], 54, 2, '<B'), 1),
        ('accessory', accessory, 0),
        ('accessory-slot-oob', patched(accessory, acc_start, 255, '<B'), 1),
        ('accessory-link-oob', patched(accessory, acc_key + 8, 0xffffffff), 1),
        ('accessory-parent-missing', patched(accessory, acc_key + 13, 17), 0),
        ('accessory-truncated', accessory[:acc_key + 20], 1),
        ('accessory-duplicate-slot', accessory[:244] + b'\2' + accessory[245:345] * 2 +
         accessory[acc_start:acc_end] * 2 + accessory[acc_end:], 1),
    ]
    for label, data, warnings in cases:
        if args.case and label not in args.case:
            continue
        project = folder / (label + '.pmm')
        project.write_bytes(data)
        run(label, [project, '--project-import', folder / (label + '-saved.pmm')], warnings)
    print('Artifacts: ' + str(folder), flush=True)
    return int(any(result['exit'] != 0 for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
