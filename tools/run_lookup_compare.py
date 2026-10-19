"""Serial, hidden-window before/after rendering of frozen benchmark builds."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import time


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding='utf-8')


def stable_playback(csv_path, requested_seconds=15):
    # The startup message pump can consume the nominal wall-clock warmup.
    # Exclude the same initial 5 seconds; longer capped runs retain more frames.
    with csv_path.open(newline='') as stream:
        rows = [r for r in csv.DictReader(stream) if float(r['end_ms']) >= 5000]
    if len(rows) < 2:
        raise RuntimeError('Not enough presented frames after startup exclusion')
    seconds = (float(rows[-1]['end_ms']) - float(rows[0]['end_ms'])) / 1000
    minimum_seconds = (requested_seconds - 5) * 0.9
    if seconds < minimum_seconds or any(int(r['presents']) != 1 or int(r['draws']) == 0 for r in rows):
        raise RuntimeError('Invalid steady playback interval')
    return dict(fps=sum(int(r['presents']) for r in rows[1:]) / seconds, seconds=seconds,
                frames=len(rows)-1, excluded_initial_ms=5000, minimum_seconds=minimum_seconds,
                scene_start_frame=int(rows[0]['scene_frame']), scene_end_frame=int(rows[-1]['scene_frame']),
                draw_counts=sorted({int(r['draws']) for r in rows}),
                primitive_counts=sorted({int(r['primitives']) for r in rows}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', choices=['image', 'playback'], required=True)
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--cycles', type=int, default=2)
    parser.add_argument('--cpu-percent', type=float, help='Windows job hard cap as percent of system CPU')
    parser.add_argument('--logical-cpus', help='Comma-separated fixed logical CPU indices, required with --cpu-percent')
    args = parser.parse_args()
    if bool(args.cpu_percent is not None) != bool(args.logical_cpus):
        parser.error('--cpu-percent and --logical-cpus must be supplied together')
    if args.cpu_percent is not None and not 0.01 <= args.cpu_percent <= 100:
        parser.error('--cpu-percent must be in [0.01, 100]')
    if args.cycles < 1:
        parser.error('--cycles must be positive')
    if args.phase == 'playback' and not 15 <= args.seconds <= 300:
        parser.error('The playback protocol requires --seconds in [15, 300]')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(phase=args.phase, seconds=args.seconds, cycles=args.cycles if args.phase == 'playback' else 1,
                    scene=str(args.scene), script_sha256=digest(Path(__file__)),
                    viewport='960x540', processor='cpu', warmup_seconds=5,
                    note='Playback counts successful Present calls; it does not measure displayed refreshes.',
                    runs=[], assets={})
    if args.cpu_percent is not None:
        import limited_process
        logical_cpus = [int(value) for value in args.logical_cpus.split(',')]
        manifest['resource_limit'] = dict(system_cpu_percent=args.cpu_percent, logical_cpus=logical_cpus,
                                          helper_sha256=digest(Path(limited_process.__file__)))
    for path in sorted(args.scene.parent.parent.rglob('*')):
        if path.is_file():
            manifest['assets'][str(path)] = digest(path)
    for mode in ('native', 'mme'):
        snapshots = {}
        for label in ('before', 'after'):
            source = args.artifacts / f'{label}-{mode}'
            snapshots[label] = {str(p.relative_to(source)): digest(p)
                                for p in source.rglob('*') if p.is_file()}
        changed = {p for p in snapshots['before'] if
                   snapshots['before'].get(p) != snapshots['after'].get(p)}
        expected = {'MikuMikuDance.exe' if mode == 'mme' else 'frame_benchmark.exe'}
        if changed != expected or snapshots['before'].keys() != snapshots['after'].keys():
            raise RuntimeError(f'{mode}: runtime differs beyond executable: {changed}')
        manifest[mode] = snapshots
    save(args.output / 'manifest.json', manifest)
    # The native runtime rewrites vtable entries; the current playback observer
    # is validated only for MME's stable device proxy. Native uses drained frames.
    modes = ('native', 'mme') if args.phase == 'image' else ('mme',)
    for mode in modes:
        sequence = ['before', 'after', 'after', 'before'] if args.phase == 'image' else ['before', 'after', 'after', 'before'] * args.cycles
        for index, label in enumerate(sequence):
            name = f'{mode}-{index:02d}-{label}'
            runtime = args.output / (name + '-runtime')
            shutil.copytree(args.artifacts / f'{label}-{mode}', runtime)
            local = runtime / 'appdata'
            local.mkdir()
            env = os.environ.copy()
            env['LOCALAPPDATA'] = str(local)
            env['APPDATA'] = str(local)
            exe = runtime / ('MikuMikuDance.exe' if mode == 'mme' else 'frame_benchmark.exe')
            result_path = args.output / (name + '.json')
            log = args.output / (name + '.log')
            command = [str(exe), str(args.scene), '--viewport', '960x540', '--processor', 'cpu', '--json', str(result_path)]
            if args.phase == 'image':
                command += ['--throughput', '--warmup', '30', '--samples', '120', str(args.output / (name + '.png'))]
            else:
                command += ['--playback-seconds', str(args.seconds)]
            run = dict(name=name, mode=mode, label=label, command=command, log=str(log),
                       exe_sha256=digest(exe), started_at=time.strftime('%Y-%m-%dT%H:%M:%S'),
                       environment={k: v for k, v in env.items() if k.startswith(('OMP_', 'MIKUMIKUSTUDIO_', 'MMS_', 'MMN_'))})
            manifest['runs'].append(run)
            save(args.output / 'manifest.json', manifest)
            with log.open('w', encoding='utf-8') as stream:
                try:
                    if args.cpu_percent is not None:
                        run['resource_telemetry'] = {}
                        gpu_log = args.output / (name + '-gpu.csv')
                        with gpu_log.open('w', encoding='utf-8') as gpu_stream:
                            gpu = None
                            try:
                                gpu = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,name,utilization.gpu,utilization.memory,clocks.gr,clocks.mem,temperature.gpu,power.draw',
                                                        '--format=csv', '--loop=1'], stdout=gpu_stream,
                                                       stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                                run['gpu_telemetry'] = str(gpu_log)
                                run['returncode'] = limited_process.run(command, runtime, env, stream, 600,
                                                                       args.cpu_percent, logical_cpus, run['resource_telemetry'])
                            finally:
                                if gpu is not None:
                                    if gpu.poll() is None:
                                        gpu.terminate()
                                    run['gpu_logger_returncode'] = gpu.wait(timeout=10)
                    else:
                        result = subprocess.run(command, cwd=runtime, env=env, stdout=stream,
                                                stderr=subprocess.STDOUT, timeout=180,
                                                creationflags=subprocess.CREATE_NO_WINDOW)
                        run['returncode'] = result.returncode
                except subprocess.TimeoutExpired:
                    run['error'] = 'timeout; only this benchmark child was stopped'
                    save(args.output / 'manifest.json', manifest)
                    raise
                except Exception as error:
                    run['error'] = str(error)
                    save(args.output / 'manifest.json', manifest)
                    raise
            if result_path.exists():
                run['result'] = json.loads(result_path.read_text(encoding='utf-8'))
            save(args.output / 'manifest.json', manifest)
            if args.phase == 'playback' and run['returncode'] == 0:
                try:
                    run['steady'] = stable_playback(Path(str(result_path) + '.csv'), args.seconds)
                except Exception as error:
                    run['analysis_error'] = str(error)
                    manifest['incomplete'] = True
                    save(args.output / 'manifest.json', manifest)
                    raise
            save(args.output / 'manifest.json', manifest)
            if run['returncode'] or ('result' not in run):
                raise RuntimeError(f'{name} failed; see {log}')
            if args.phase == 'playback' and not run['result']['valid']:
                raise RuntimeError(f'{name}: invalid playback sample')
            metric = run['result']['fps'] if args.phase == 'playback' else run['result']['timing']['drained_batch_ms_per_frame']
            print(f'{name}: {metric:.4f}', flush=True)
    summary = {}
    if args.phase == 'playback':
        for mode in modes:
            record = {}
            for label in ('before', 'after'):
                values = [r['steady']['fps'] for r in manifest['runs'] if r['mode'] == mode and r['label'] == label]
                median = statistics.median(values)
                record[label] = dict(samples=values, median=median, minimum=min(values), maximum=max(values),
                                     range_percent=100 * (max(values) - min(values)) / median)
            record['change_percent'] = (record['after']['median'] / record['before']['median'] - 1) * 100
            record['unstable'] = any(record[label]['range_percent'] > 5 for label in ('before', 'after'))
            record['abba_change_percent'] = []
            runs = [r for r in manifest['runs'] if r['mode'] == mode]
            for begin in range(0, len(runs), 4):
                a, b, c, d = [r['steady']['fps'] for r in runs[begin:begin + 4]]
                record['abba_change_percent'].append(((b + c) / (a + d) - 1) * 100)
            summary[mode] = record
    save(args.output / 'summary.json', summary)


if __name__ == '__main__':
    main()
