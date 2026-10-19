"""Check actual material draws with and without native MME in hidden hosts."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

from mme_display_fixtures import fixture


def main():
    executable, mme, output = map(lambda value: Path(value).resolve(), sys.argv[1:4])
    run = output / str(time.time_ns())
    results = []
    for name, use_mme in [('native', False), ('mme', True)]:
        directory = run / name
        directory.mkdir(parents=True)
        host = directory / 'MikuMikuDance.exe'
        shutil.copy2(executable, host)
        if use_mme:
            for dll in ['d3d9.dll', 'MMEffect.dll', 'MMHack.dll']:
                shutil.copy2(mme / dll, directory / dll)
        (directory / 'Data').mkdir()
        (directory / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
        model = directory / 'alpha-order.pmx'
        model.write_bytes(fixture(alpha_order=True))
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('MIKUMIKUSTUDIO_', 'MMS_MME_'))}
        env['LOCALAPPDATA'] = str(directory / 'Local')
        env['APPDATA'] = str(directory / 'Roaming')
        env['MIKUMIKUSTUDIO_AUTOSAVE_CONFIG'] = str(directory / 'autosave.ini')
        if not use_mme:
            env['MMS_MME_ORDER_NATIVE'] = '1'
        command = [str(host), str(model), '--alpha-order', str(directory / 'pixels')]
        with (directory / 'result.log').open('wb') as log:
            result = subprocess.run(command, cwd=directory, env=env, stdout=log,
                                    stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=45)
        summary = {'case': name, 'exit_code': result.returncode, 'command': command,
                   'exe_sha256': hashlib.sha256(host.read_bytes()).hexdigest(),
                   'log': str(directory / 'result.log')}
        results.append(summary)
        print(json.dumps(summary), flush=True)
        for line in (directory / 'result.log').read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith(('FAIL', 'DRAW', 'PIXELS')):
                print(line, flush=True)
    (run / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return int(any(result['exit_code'] != 0 for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
