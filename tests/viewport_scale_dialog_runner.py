"""Run the real modal-resolution/FPS regression in disposable hidden hosts."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import time

source, output, mme_source, model = map(lambda p: Path(p).resolve(), sys.argv[1:5])
motion = Path(sys.argv[5]).resolve() if len(sys.argv) > 5 else None
root = output / str(time.time_ns())
results = []
for mme in (False, True):
    directory = root / ("mme" if mme else "native")
    (directory / "Data").mkdir(parents=True)
    host = directory / "MikuMikuDance.exe"
    shutil.copy2(source, host)
    (directory / "Data/mmconfig.ini").write_text("40\n40\n1000\n740\n0\n640\n480\n250\n2\n")
    if mme:
        for name in ("d3d9.dll", "MMEffect.dll", "MMHack.dll"):
            shutil.copy2(mme_source / name, directory / name)
    env = os.environ.copy()
    env.update(
        LOCALAPPDATA=str(directory / "Local"),
        APPDATA=str(directory / "Roaming"),
        MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(directory / "autosave.ini"),
        MMS_ANTIALIAS_CONFIG=str(directory / "aa.ini"),
        MMS_MME_ORDER_NATIVE="0" if mme else "1",
        MMS_VIEWPORT_DIALOG="1",
    )
    if motion:
        env["MMS_SCALE_DANCE_VMD"] = str(motion)
    with (directory / "result.log").open("wb") as log:
        result = subprocess.run(
            [str(host), str(model), "--viewport-scale", str(directory)],
            cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=180,
        )
    results.append(dict(mode="mme" if mme else "native", exit_code=result.returncode,
                        directory=str(directory)))
    (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results[-1]), flush=True)
    if result.returncode:
        print((directory / "result.log").read_text(encoding="utf-8", errors="replace"))
        raise SystemExit(result.returncode)
