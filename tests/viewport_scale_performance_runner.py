"""Measure real MME previews in a disposable host, with optional target tracing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('executable', 'mme', 'model', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--accessory', type=Path)
    parser.add_argument('--mapping', type=Path)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--trace-targets', action='store_true')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--project-settings', action='store_true')
    parser.add_argument('--gpu-timing', action='store_true')
    parser.add_argument('--no-vertex-cache', action='store_true')
    parser.add_argument('--verify-vertex-cache', action='store_true')
    parser.add_argument('--camera-motion', action='store_true')
    parser.add_argument('--no-outline-cache', action='store_true')
    parser.add_argument('--cpu-skinning', action='store_true')
    parser.add_argument('--no-file-watch', action='store_true')
    parser.add_argument('--gpu-draw-timing', action='store_true')
    parser.add_argument('--gpu-samples', type=int, default=16)
    parser.add_argument('--seconds', type=float, default=1.5)
    args = parser.parse_args()
    if not 1.5 <= args.seconds <= 30:
        parser.error('--seconds must be between 1.5 and 30')
    if not 4 <= args.gpu_samples <= 64:
        parser.error('--gpu-samples must be between 4 and 64')
    if args.gpu_draw_timing and not args.gpu_timing:
        parser.error('--gpu-draw-timing requires --gpu-timing')
    sources = [args.model.resolve()]
    if args.model.suffix.lower() == '.pmm' and args.model.with_suffix('.emm').is_file():
        sources.append(args.model.with_suffix('.emm').resolve())
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    host = root / 'MikuMikuDance.exe'
    shutil.copy2(args.executable, host)
    if not args.native:
        for name in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
            shutil.copy2(args.mme / name, root / name)
    (root / 'Data').mkdir()
    (root / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n', encoding='ascii')
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root / 'Local'), APPDATA=str(root / 'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(root / 'autosave.ini'),
               MMS_ANTIALIAS_CONFIG=str(root / 'aa.ini'), MMS_NVIDIA_AA_FIX='0', MMS_VIEWPORT_PERF='1')
    for name in ('MMS_SCALE_TRACE_TARGETS', 'MMS_SCALE_QUICK', 'MMS_MME_ORDER_NATIVE', 'MMS_SCALE_ACCESSORY', 'MMS_SCALE_EMM', 'MMS_SCALE_CAPTURE', 'MMS_SCALE_PROJECT_SETTINGS', 'MMS_SCALE_GPU_TIMING', 'MMS_SCALE_GPU_SAMPLES', 'MMS_SCALE_GPU_DRAWS'):
        env.pop(name, None)
    if args.native:
        env['MMS_MME_ORDER_NATIVE'] = '1'
    env.pop('MMS_SCALE_NO_VERTEX_CACHE', None)
    if args.no_vertex_cache:
        env['MMS_SCALE_NO_VERTEX_CACHE'] = '1'
    env.pop('MMS_SCALE_VERIFY_VERTEX_CACHE', None)
    if args.verify_vertex_cache:
        env['MMS_SCALE_VERIFY_VERTEX_CACHE'] = '1'
    env.pop('MMS_SCALE_CAMERA_MOTION', None)
    if args.camera_motion:
        env['MMS_SCALE_CAMERA_MOTION'] = '1'
    env.pop('MMS_SCALE_NO_OUTLINE_CACHE', None)
    if args.no_outline_cache:
        env['MMS_SCALE_NO_OUTLINE_CACHE'] = '1'
    env.pop('MMS_SCALE_CPU_SKINNING', None)
    if args.cpu_skinning:
        env['MMS_SCALE_CPU_SKINNING'] = '1'
    env.pop('MMS_NO_MME_FILE_WATCH', None)
    if args.no_file_watch:
        env['MMS_NO_MME_FILE_WATCH'] = '1'
    if args.trace_targets:
        env['MMS_SCALE_TRACE_TARGETS'] = '1'
    if args.quick:
        env['MMS_SCALE_QUICK'] = '1'
    env['MMS_SCALE_SECONDS'] = str(args.seconds)
    if args.capture:
        env['MMS_SCALE_CAPTURE'] = '1'
    if args.project_settings:
        env['MMS_SCALE_PROJECT_SETTINGS'] = '1'
    if args.gpu_timing:
        env['MMS_SCALE_GPU_TIMING'] = '1'
        env['MMS_SCALE_GPU_SAMPLES'] = str(args.gpu_samples)
    if args.gpu_draw_timing:
        env['MMS_SCALE_GPU_DRAWS'] = '1'
    if args.accessory:
        env['MMS_SCALE_ACCESSORY'] = str(args.accessory.resolve())
    if args.mapping:
        env['MMS_SCALE_EMM'] = str(args.mapping.resolve())
    with (root / 'result.log').open('wb') as log:
        result = subprocess.run([str(host), str(args.model.resolve()), '--viewport-scale', str(root)],
                                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW, timeout=max(120, int(args.seconds * 16 + 90)))
    summary = {'exit_code': result.returncode, 'model': str(args.model.resolve()),
               'seconds_per_block': args.seconds, 'project_settings': args.project_settings,
               'gpu_timing': args.gpu_timing, 'gpu_samples_per_block': args.gpu_samples if args.gpu_timing else 0,
               'vertex_cache_disabled': args.no_vertex_cache,
               'cpu_skinning': args.cpu_skinning,
               'file_watch_disabled': args.no_file_watch,
               'vertex_cache_verified': args.verify_vertex_cache,
               'camera_motion': args.camera_motion,
               'outline_cache_disabled': args.no_outline_cache,
               'gpu_per_draw_timing': args.gpu_draw_timing,
               'environment': {key: env[key] for key in ('OMP_NUM_THREADS', 'OMP_WAIT_POLICY') if key in env},
               'native': args.native, 'accessory': str(args.accessory) if args.accessory else None,
               'sources_unchanged': before == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
               'executable_sha256': hashlib.sha256(args.executable.read_bytes()).hexdigest(),
               'log': str(root / 'result.log')}
    (root / 'result.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    for line in (root / 'result.log').read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith(('SCALE_', 'FAIL', 'phase=')):
            print(line, flush=True)
    return result.returncode or int(not summary['sources_unchanged'])


if __name__ == '__main__':
    raise SystemExit(main())
