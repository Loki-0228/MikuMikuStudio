"""Verify versioned project/EMM pairing against the real MME DLLs in a hidden host."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time
from mme_display_fixtures import fixture
from mme_emm_paths_runner import EFFECT


def main():
    executable, mme, output = (Path(value).resolve() for value in sys.argv[1:4])
    run = output / str(time.time_ns())
    results = []
    for suffix in ('pmm1', 'pmm11', 'PmM111'):
        directory = run / suffix
        directory.mkdir(parents=True)
        shutil.copy2(executable, directory / 'MikuMikuDance.exe')
        for dll in ('d3d9.dll', 'MMEffect.dll', 'MMHack.dll'):
            shutil.copy2(mme / dll, directory / dll)
        (directory / 'Data').mkdir()
        (directory / 'Data/mmconfig.ini').write_text('40\n40\n1000\n740\n0\n640\n480\n250\n1\n')
        (directory / 'MMEffect.ini').write_text('[System]\nSkipValidation=true\nEMMAutoSave=true\n')
        model = directory / 'model.pmx'
        model.write_bytes(fixture())
        effect = directory / 'assigned.fx'
        effect.write_text(EFFECT, encoding='ascii')
        project = directory / ('scene.' + suffix)
        project.write_bytes(b'Notification fixture; native PMM parser tested separately.\n')
        assignment = project.with_suffix('.emm' + project.suffix[4:])
        assignment.write_text(f'[Info]\nVersion = 3\n[Object]\nPmd1 = {model}\n'
                              f'[Effect]\nDefault = none\nPmd1 = {effect}\nPmd1.show = true\n')
        env = dict(os.environ, MMS_PMM_VERSION_TEST='1', LOCALAPPDATA=str(directory/'Local'),
                   APPDATA=str(directory/'Roaming'), MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(directory/'autosave.ini'))
        log = directory / 'result.log'
        with log.open('wb') as stream:
            result = subprocess.run([str(directory/'MikuMikuDance.exe'), str(model), '--emm-load', str(project)],
                                    cwd=directory, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=45)
        results.append(dict(case=suffix, passed=result.returncode == 0, exit=result.returncode, log=str(log)))
        print(json.dumps(results[-1]), flush=True)
        print(log.read_text(encoding='utf-8', errors='replace')[-1000:], flush=True)
    (run/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return int(any(not item['passed'] for item in results))


if __name__ == '__main__':
    raise SystemExit(main())
