"""Run scaled native/MME model previews in disposable hidden host directories."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import time
from native_shadow_cycle import fixture

source = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve() / str(time.time_ns())
mme = Path(sys.argv[3]).resolve()
for mode in ('native', 'mme'):
    directory = root / mode
    (directory / 'Data').mkdir(parents=True)
    host = directory / 'MikuMikuDance.exe'
    shutil.copy2(source, host)
    if mode == 'mme':
        for name in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
            shutil.copy2(mme / name, directory / name)
    (directory / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n2\n')
    model = directory / 'scale-model.pmx'
    fixture(model)
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(directory / 'Local'), APPDATA=str(directory / 'Roaming'),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(directory / 'autosave.ini'))
    if mode == 'native':
        env['MMS_MME_ORDER_NATIVE'] = '1'
    else:
        env.pop('MMS_MME_ORDER_NATIVE', None)
    with (directory / 'result.log').open('wb') as log:
        run = subprocess.run([str(host), str(model), '--viewport-scale', str(directory)],
                             cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                             creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    print(directory, flush=True)
    print((directory / 'result.log').read_text(encoding='utf-8', errors='replace'), flush=True)
    if run.returncode:
        raise SystemExit(run.returncode)
