"""Compare frozen MMS builds concurrently within a combined two-percent CPU cap."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import threading
import time

import limited_process
from run_lookup_compare import digest, save, stable_playback


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=90)
    args = parser.parse_args()
    if not 45 <= args.seconds <= 300:
        parser.error('--seconds must be in [45, 300]')
    args.output.mkdir(parents=True, exist_ok=False)
    packages = {}
    for label in ('before', 'after'):
        source = args.artifacts / f'{label}-mme'
        packages[label] = {str(p.relative_to(source)): digest(p) for p in source.rglob('*') if p.is_file()}
    assert packages['before'].keys() == packages['after'].keys()
    assert {p for p in packages['before'] if packages['before'][p] != packages['after'][p]} == {'MikuMikuDance.exe'}
    topology = limited_process.topology()
    selected = [next(p for p in topology if p['logical'] == logical) for logical in (4, 6)]
    assert selected[0]['core'] != selected[1]['core']
    assert selected[0]['efficiency'] == selected[1]['efficiency'] == max(p['efficiency'] for p in topology)
    manifest = dict(protocol='simultaneous_background_playback', seconds=args.seconds,
                    system_cpu_percent_per_process=1, system_cpu_percent_combined=2,
                    selected_cpus=selected, script_sha256=digest(Path(__file__)),
                    helper_sha256=digest(Path(limited_process.__file__)),
                    packages=packages, pairs=[], assets={})
    for path in sorted(args.scene.parent.parent.rglob('*')):
        if path.is_file():
            manifest['assets'][str(path)] = digest(path)
    manifest_path = args.output / 'manifest.json'
    save(manifest_path, manifest)
    # Four paired runs balance which version occupies each physical P core.
    for index, before_cpu in enumerate((4, 6, 6, 4)):
        after_cpu = 6 if before_cpu == 4 else 4
        pair = dict(index=index, cores={'before': before_cpu, 'after': after_cpu}, runs={})
        manifest['pairs'].append(pair)
        for label in ('before', 'after'):
            name = f'pair-{index:02d}-{label}'
            runtime = args.output / (name + '-runtime')
            shutil.copytree(args.artifacts / f'{label}-mme', runtime)
            local = runtime / 'appdata'
            local.mkdir()
            result = args.output / (name + '.json')
            command = [str(runtime / 'MikuMikuDance.exe'), str(args.scene), '--viewport', '960x540',
                       '--processor', 'cpu', '--json', str(result), '--playback-seconds', str(args.seconds)]
            pair['runs'][label] = dict(name=name, command=command, runtime=str(runtime),
                                      result_path=str(result), exe_sha256=digest(runtime / 'MikuMikuDance.exe'),
                                      logical_cpu=pair['cores'][label], resource_telemetry={})
        save(manifest_path, manifest)
        barrier = threading.Barrier(2)
        def worker(label):
            run = pair['runs'][label]
            runtime = Path(run['runtime'])
            env = os.environ.copy()
            env['LOCALAPPDATA'] = env['APPDATA'] = str(runtime / 'appdata')
            run['environment'] = {k: v for k, v in env.items() if k.startswith(('OMP_', 'MIKUMIKUSTUDIO_', 'MMS_', 'MMN_'))}
            try:
                with (args.output / (run['name'] + '.log')).open('w', encoding='utf-8') as stream:
                    run['returncode'] = limited_process.run(run['command'], runtime, env, stream, 900,
                                                            1, [run['logical_cpu']], run['resource_telemetry'], barrier)
                run['finished_at_unix_ns'] = time.time_ns()
                result = Path(run['result_path'])
                run['result'] = json.loads(result.read_text(encoding='utf-8'))
                run['steady'] = stable_playback(Path(str(result) + '.csv'), args.seconds)
                assert run['returncode'] == 0 and run['result']['valid']
                assert run['steady']['draw_counts'] == [1118]
                assert run['steady']['primitive_counts'] == [6963591]
                run['result_written_at_unix_ns'] = result.stat().st_mtime_ns
            except Exception as error:
                run['error'] = str(error)
                barrier.abort()
                raise
        gpu_log = args.output / f'pair-{index:02d}-gpu.csv'
        pair['gpu_telemetry'] = str(gpu_log)
        try:
            with gpu_log.open('w', encoding='utf-8') as stream:
                gpu = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,name,utilization.gpu,utilization.memory,clocks.gr,clocks.mem,temperature.gpu,power.draw',
                                        '--format=csv', '--loop=1'], stdout=stream, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    with ThreadPoolExecutor(max_workers=2) as pool:
                        tasks = [pool.submit(worker, label) for label in ('before', 'after')]
                        errors = []
                        for task in tasks:
                            try:
                                task.result()
                            except Exception as error:
                                errors.append(str(error))
                        if errors:
                            raise RuntimeError('; '.join(errors))
                finally:
                    if gpu.poll() is None:
                        gpu.terminate()
                    pair['gpu_logger_returncode'] = gpu.wait(timeout=10)
            before, after = [pair['runs'][label] for label in ('before', 'after')]
            pair['resume_difference_ms'] = abs(before['resource_telemetry']['resumed_at_unix_ns'] - after['resource_telemetry']['resumed_at_unix_ns']) / 1e6
            # Existing frozen executables do not timestamp sample start in UTC.
            # Estimate overlap from result file completion time minus duration;
            # retain this limitation instead of claiming lockstep scene playback.
            intervals = [(r['result_written_at_unix_ns'] / 1e9 - r['result']['seconds'] + 5,
                          r['result_written_at_unix_ns'] / 1e9) for r in (before, after)]
            overlap = max(0, min(r[1] for r in intervals) - max(r[0] for r in intervals))
            pair['estimated_steady_overlap_seconds'] = overlap
            pair['overlap_estimate_note'] = 'Result-file timestamp proxy; includes final record-write delay, not an exact rendering timestamp.'
            if pair['resume_difference_ms'] > 100 or overlap < (args.seconds - 5) * .9:
                raise RuntimeError('Insufficient concurrent sample overlap')
            pair['change_percent'] = (after['steady']['fps'] / before['steady']['fps'] - 1) * 100
            save(manifest_path, manifest)
            print(f"pair-{index:02d}: {before['steady']['fps']:.4f} -> {after['steady']['fps']:.4f}; overlap approximately {overlap:.1f}s", flush=True)
        except Exception as error:
            pair['error'] = str(error)
            manifest['incomplete'] = True
            save(manifest_path, manifest)
            raise
    summary = {}
    for label in ('before', 'after'):
        values = [p['runs'][label]['steady']['fps'] for p in manifest['pairs']]
        median = statistics.median(values)
        summary[label] = dict(samples=values, median=median, minimum=min(values), maximum=max(values),
                              range_percent=100 * (max(values) - min(values)) / median)
    summary['change_percent'] = (summary['after']['median'] / summary['before']['median'] - 1) * 100
    summary['paired_changes_percent'] = [p['change_percent'] for p in manifest['pairs']]
    summary['unstable'] = any(summary[label]['range_percent'] > 5 for label in ('before', 'after'))
    save(args.output / 'summary.json', summary)


if __name__ == '__main__':
    main()
