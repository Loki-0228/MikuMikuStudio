"""Run real MMS release executables with a PMM concurrently.

Uses the existing production pump log and read-only process state. The metric
is completed FrameDriver calls / observed time, NOT successful Present FPS.
Source-backed WM_COMMAND messages exercise the normal playback handlers in
hidden integration-test windows; no foreground input or process memory writes.
The two supported release hashes pin every memory layout used here.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import threading
import time

import capstone
import pefile
import limited_process as lp
from run_lookup_compare import digest, save

HASHES = {
    'before': 'd565e126c61038d7492792ae024d2c6c21faa9e421ba1a925a790f8f22c251a7',
    'after': '90cf474dcc0f004f757828369483979d9d00775e490fd5543c6e9a6dc7d500d6',
}
u = c.WinDLL('user32', use_last_error=True)


def ua(name, result, *args):
    fn = getattr(u, name)
    fn.restype, fn.argtypes = result, args
    return fn


open_process = lp.api('OpenProcess', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
rpm = lp.api('ReadProcessMemory', w.BOOL, w.HANDLE, c.c_void_p, c.c_void_p,
             c.c_size_t, c.POINTER(c.c_size_t))
get_item = ua('GetDlgItem', w.HWND, w.HWND, c.c_int)
post = ua('PostMessageW', w.BOOL, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
send = ua('SendMessageTimeoutW', c.c_size_t, w.HWND, w.UINT, w.WPARAM,
          w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t))
visible = ua('IsWindowVisible', w.BOOL, w.HWND)
iconic = ua('IsIconic', w.BOOL, w.HWND)
window_pid = ua('GetWindowThreadProcessId', w.DWORD, w.HWND, c.POINTER(w.DWORD))
resize = ua('SetWindowPos', w.BOOL, w.HWND, w.HWND, c.c_int, c.c_int,
            c.c_int, c.c_int, w.UINT)


class Module(c.Structure):
    _fields_ = [('size', w.DWORD), ('id', w.DWORD), ('pid', w.DWORD),
                ('global_usage', w.DWORD), ('usage', w.DWORD), ('base', c.c_void_p),
                ('bytes', w.DWORD), ('handle', w.HMODULE),
                ('name', w.WCHAR * 256), ('path', w.WCHAR * 260)]


snap_modules = lp.api('CreateToolhelp32Snapshot', w.HANDLE, w.DWORD, w.DWORD)
first_module = lp.api('Module32FirstW', w.BOOL, w.HANDLE, c.POINTER(Module))
next_module = lp.api('Module32NextW', w.BOOL, w.HANDLE, c.POINTER(Module))


def modules(pid):
    handle = snap_modules(8, pid)
    if handle == c.c_void_p(-1).value:
        return []
    try:
        item = Module()
        item.size = c.sizeof(item)
        result = []
        ok = first_module(handle, c.byref(item))
        while ok:
            result.append(dict(name=item.name, path=item.path, base=item.base))
            ok = next_module(handle, c.byref(item))
        return result
    finally:
        lp.close(handle)


def read(handle, address, fmt):
    size = struct.calcsize('<' + fmt)
    buffer, got = c.create_string_buffer(size), c.c_size_t()
    lp.checked(rpm(handle, address, buffer, size, c.byref(got)))
    if got.value != size:
        raise RuntimeError('Incomplete process state read')
    values = struct.unpack('<' + fmt, buffer)
    return values[0] if len(values) == 1 else values


def block_rva(exe):
    """Decode the g_Block RIP load in the exported ExpGetFrameTime function."""
    with pefile.PE(str(exe)) as pe:
        rva = next(e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols
                   if e.name == b'ExpGetFrameTime')
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        decoder.detail = True
        instructions = list(decoder.disasm(pe.get_data(rva, 64), rva))
        ins = instructions[0]
        if ins.mnemonic != 'mov' or ins.operands[1].type != capstone.x86.X86_OP_MEM:
            raise RuntimeError('Unrecognized release export')
        memory = ins.operands[1].mem
        if memory.base != capstone.x86.X86_REG_RIP:
            raise RuntimeError('Unrecognized g_Block reference')
        return dict(rva=ins.address + ins.size + memory.disp,
                    disassembly=[f'{i.address:x} {i.mnemonic} {i.op_str}' for i in instructions])


def state(handle, app, pid):
    hwnd = read(handle, app + 661192, 'Q')
    owner = w.DWORD()
    window_pid(hwnd, c.byref(owner))
    if owner.value != pid:
        raise RuntimeError('Application window ownership mismatch')
    result = dict(hwnd=hwnd, hidden=not bool(visible(hwnd)), minimized=bool(iconic(hwnd)),
                  playing=read(handle, app + 877, 'B'),
                  scene_seconds=read(handle, app + 652608, 'f'),
                  fps_limit=read(handle, app + 661764, 'f'),
                  fps_display=read(handle, app + 868, 'I'),
                  message_seen=read(handle, app + 663064, 'I'),
                  stereo=read(handle, app + 663053, 'B'),
                  viewport=read(handle, app + 663020, '4i'),
                  render_size=read(handle, app + 661752, '2I'),
                  camera=read(handle, app + 832, '5f'),
                  camera_distance=read(handle, app + 661760, 'f'),
                  ground_shadow=read(handle, app + 5096, 'B'),
                  self_shadow=read(handle, app + 662992, 'i'),
                  playback_physics_mode=read(handle, app + 662860, 'i'))
    slots = read(handle, app + 3048, '255Q')
    result['models'] = sum(bool(p) for p in slots)
    renderer = read(handle, app + 661216, 'Q')
    result['device'] = read(handle, renderer + 240064, 'Q') if renderer else 0
    return result


def set_text(hwnd, text):
    buffer = c.create_unicode_buffer(text)
    result = c.c_size_t()
    lp.checked(send(hwnd, 0xC, 0, c.addressof(buffer), 2, 20000, c.byref(result)))
    if not result.value:
        raise RuntimeError('Playback control text rejected')


def pump_records(path):
    if not path.exists():
        return []
    text = path.read_text(encoding='ascii')
    return [dict(pumps=int(m[0]), pump_ms=float(m[1]), fps_limit=float(m[2]))
            for m in re.findall(r'loops=\d+ msgs=\d+ pump=(\d+) pumpMs=([\d.]+) sleepMs=[\d.]+ fpsLimit=([\d.]+)\n', text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--mme-runtime', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--pairs', type=int, choices=(1, 2, 4), default=4)
    parser.add_argument('--uncapped', action='store_true',
                        help='Remove CPU rate limits and affinity overrides from both releases')
    args = parser.parse_args()
    if not 45 <= args.seconds <= 300:
        parser.error('Sampling requires 45..300 seconds')
    args.output.mkdir(parents=True, exist_ok=False)
    for label in HASHES:
        if digest(getattr(args, label)) != HASHES[label]:
            raise RuntimeError(f'Unvalidated release hash: {label}')
    topo = lp.topology()
    cores = [next(t for t in topo if t['logical'] == n) for n in (4, 6)]
    if cores[0]['core'] == cores[1]['core'] or any(t['efficiency'] != max(x['efficiency'] for x in topo) for t in cores):
        raise RuntimeError('Expected distinct performance cores')
    manifest = dict(metric='production_completed_frame_driver_calls_per_second',
                    note='Existing production pump log. Not successful Present FPS or displayed FPS. No injected DLL or modified EXE.',
                    scene=str(args.scene), scene_sha256=digest(args.scene),
                    cpu_percent_each=None if args.uncapped else 1,
                    cpu_percent_total=None if args.uncapped else 2, seconds=args.seconds,
                    poll_interval_seconds=.1, selected_cores=cores,
                    script_sha256=digest(Path(__file__)), helper_sha256=digest(Path(lp.__file__)),
                    executables={label: dict(path=str(getattr(args, label)), sha256=HASHES[label],
                                            block=block_rva(getattr(args, label))) for label in HASHES}, pairs=[])
    manifest_path = args.output / 'manifest.json'
    save(manifest_path, manifest)
    for index, before_core in enumerate((4, 6, 6, 4)[:args.pairs]):
        pair = dict(index=index, runs={})
        manifest['pairs'].append(pair)
        controls, handles, barrier = {}, {}, threading.Barrier(2)
        for label in HASHES:
            runtime = args.output / f'{index:02d}-{label}'
            shutil.copytree(args.runtime, runtime)
            for name in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll', 'MMEffect.ini'):
                shutil.copy2(args.mme_runtime / name, runtime / name)
            shutil.copy2(getattr(args, label), runtime / 'MikuMikuDance.exe')
            resources = {str(p.relative_to(runtime)): digest(p) for p in runtime.rglob('*')
                         if p.is_file() and p.name != 'MikuMikuDance.exe'}
            appdata = runtime / 'appdata' / 'MikuMikuStudio'
            appdata.mkdir(parents=True)
            (appdata / 'processor.ini').write_text('[Vertices]\nMode=cpu\n', encoding='ascii')
            pair['runs'][label] = dict(runtime=str(runtime), resources=resources,
                                      core=None if args.uncapped else (before_core if label == 'before' else 10 - before_core),
                                      resource_telemetry={}, observations=[])
            controls[label] = threading.Event()
        if pair['runs']['before']['resources'] != pair['runs']['after']['resources']:
            raise RuntimeError('Runtime resource mismatch')
        def worker(label):
            run = pair['runs'][label]
            runtime = Path(run['runtime'])
            env = {k: v for k, v in os.environ.items() if not k.startswith(('MIKUMIKUSTUDIO_', 'MMS_', 'MMN_', 'OMP_'))}
            env['LOCALAPPDATA'] = env['APPDATA'] = str(runtime / 'appdata')
            env['MIKUMIKUSTUDIO_PUMP_STATS'] = str(runtime / 'pump.log')
            with (runtime / 'process.log').open('w', encoding='utf-8') as output:
                return lp.run([str(runtime / 'MikuMikuDance.exe'), str(args.scene)], runtime, env,
                              output, 1200, None if args.uncapped else 1,
                              None if args.uncapped else [run['core']], run['resource_telemetry'], barrier,
                              controls[label])
        gpu_stream = (args.output / f'{index:02d}-gpu.csv').open('w', encoding='utf-8')
        gpu = None
        try:
            gpu = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,name,utilization.gpu,utilization.memory,clocks.gr,clocks.mem,temperature.gpu,power.draw',
                                    '--format=csv', '--loop=1'], stdout=gpu_stream, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = {label: pool.submit(worker, label) for label in HASHES}
                try:
                    deadline = time.perf_counter() + 600
                    while len(handles) != 2:
                        for label, run in pair['runs'].items():
                            if futures[label].done():
                                raise RuntimeError(f'{label} exited during loading: {futures[label].result()}')
                            pid = run['resource_telemetry'].get('pid')
                            if label in handles or not pid or not (Path(run['runtime']) / 'pump.log').exists():
                                continue
                            loaded = modules(pid)
                            if not loaded:
                                continue
                            handle = open_process(0x410, False, pid)
                            lp.checked(handle)
                            try:
                                app = read(handle, loaded[0]['base'] + manifest['executables'][label]['block']['rva'], 'Q')
                                snapshot = state(handle, app, pid)
                                if snapshot['models'] != 11:
                                    raise RuntimeError(f'{label}: expected 11 models, got {snapshot["models"]}')
                                names = {p['name'].lower() for p in loaded}
                                if not {'mmeffect.dll', 'mmhack.dll'} <= names:
                                    raise RuntimeError('MME modules did not load')
                                if not snapshot['hidden'] or snapshot['minimized'] or not snapshot['device']:
                                    raise RuntimeError('Invalid hidden renderer state')
                                run['loaded'] = snapshot
                                run['modules'] = loaded
                                handles[label] = (handle, app, pid)
                            except Exception:
                                lp.close(handle)
                                raise
                        if time.perf_counter() > deadline:
                            raise RuntimeError('Timed out loading the actual project')
                        time.sleep(.2)
                    print(f'pair {index}: both real releases loaded Stage.pmm with 11 models', flush=True)
                    # Source-backed integration commands, only to windows owned by these jobs.
                    for label, (handle, app, pid) in handles.items():
                        snapshot = state(handle, app, pid)
                        hwnd = snapshot['hwnd']
                        lp.checked(resize(hwnd, None, 0, 0, 1463, 914, 0x16))
                        lp.checked(post(hwnd, 0x111, 234, 0))
                        end = get_item(hwnd, 0x19A)
                        if not end:
                            raise RuntimeError('Missing normal playback range control')
                        set_text(end, '300000')
                    for label, (handle, app, pid) in handles.items():
                        hwnd = state(handle, app, pid)['hwnd']
                        lp.checked(post(hwnd, 0x111, 408, get_item(hwnd, 408)))
                    deadline = time.perf_counter() + 60
                    while True:
                        snapshots = {label: state(*values) for label, values in handles.items()}
                        if all(s['playing'] == 1 and s['fps_limit'] == 1000 and s['scene_seconds'] > 0 for s in snapshots.values()):
                            break
                        if time.perf_counter() > deadline:
                            raise RuntimeError('Normal playback did not start')
                        time.sleep(.2)
                    warmup_until = time.perf_counter() + 10
                    while time.perf_counter() < warmup_until:
                        time.sleep(.2)
                    for label, run in pair['runs'].items():
                        run['sample_start_state'] = state(*handles[label])
                        run['initial_records'] = len(pump_records(Path(run['runtime']) / 'pump.log'))
                        run['seen_records'] = run['initial_records']
                    for field in ('viewport', 'render_size', 'camera', 'camera_distance', 'self_shadow', 'ground_shadow', 'playback_physics_mode'):
                        if pair['runs']['before']['sample_start_state'][field] != pair['runs']['after']['sample_start_state'][field]:
                            raise RuntimeError(f'Initial scene mismatch: {field}')
                    pair['sampling_started_unix_ns'] = time.time_ns()
                    save(manifest_path, manifest)
                    start = time.perf_counter()
                    while time.perf_counter() - start < args.seconds:
                        for label, run in pair['runs'].items():
                            if futures[label].done():
                                raise RuntimeError(f'{label} exited during measurement')
                            records = pump_records(Path(run['runtime']) / 'pump.log')
                            if len(records) > run['seen_records']:
                                snapshot = state(*handles[label])
                                if (snapshot['playing'] != 1 or snapshot['models'] != 11 or snapshot['minimized'] or
                                    not snapshot['hidden'] or snapshot['message_seen'] == 0 or snapshot['stereo'] != 0):
                                    raise RuntimeError('Playback/render gate changed')
                                run['observations'].append(dict(seconds=time.perf_counter() - start,
                                    record_end=len(records), new_records=records[run['seen_records']:], state=snapshot))
                                run['seen_records'] = len(records)
                        time.sleep(.1)
                    pair['sampling_finished_unix_ns'] = time.time_ns()
                    pair['wall_seconds'] = time.perf_counter() - start
                    for label, run in pair['runs'].items():
                        run['sample_end_state'] = state(*handles[label])
                        obs = run['observations']
                        if len(obs) < 3 or obs[-1]['seconds'] - obs[0]['seconds'] < args.seconds * .8:
                            raise RuntimeError('Insufficient completed production frames')
                        pumps = sum(r['pumps'] for o in obs[1:] for r in o['new_records'])
                        duration = obs[-1]['seconds'] - obs[0]['seconds']
                        run['measurement'] = dict(completed_frames=pumps, observed_seconds=duration,
                                                  frame_loop_fps=pumps / duration,
                                                  scene_start_seconds=obs[0]['state']['scene_seconds'],
                                                  scene_end_seconds=obs[-1]['state']['scene_seconds'])
                        if run['measurement']['scene_end_seconds'] <= run['measurement']['scene_start_seconds']:
                            raise RuntimeError('Scene timeline did not advance')
                    pair['change_percent'] = (pair['runs']['after']['measurement']['frame_loop_fps'] /
                                              pair['runs']['before']['measurement']['frame_loop_fps'] - 1) * 100
                    print(f'pair {index}: ' + ' / '.join(f"{label} {run["measurement"]["frame_loop_fps"]:.4f} frame loops/s" for label, run in pair['runs'].items()), flush=True)
                finally:
                    barrier.abort()
                    for event in controls.values():
                        event.set()
                    for label, future in futures.items():
                        pair['runs'][label]['returncode'] = future.result(timeout=15)
        except Exception as error:
            pair['error'] = str(error)
            manifest['incomplete'] = True
            raise
        finally:
            if gpu is not None:
                if gpu.poll() is None:
                    gpu.terminate()
                pair['gpu_logger_returncode'] = gpu.wait(timeout=10)
            gpu_stream.close()
            for handle, _, _ in handles.values():
                lp.close(handle)
            save(manifest_path, manifest)
    values = {label: [p['runs'][label]['measurement']['frame_loop_fps'] for p in manifest['pairs']] for label in HASHES}
    summary = {label: dict(values=v, median=statistics.median(v),
                          range_percent=(max(v) - min(v)) / statistics.median(v) * 100) for label, v in values.items()}
    summary['change_percent'] = (summary['after']['median'] / summary['before']['median'] - 1) * 100
    summary['metric'] = manifest['metric']
    save(args.output / 'summary.json', summary)


if __name__ == '__main__':
    main()
