"""Run an isolated, hidden three-model frozen bone-scene acceptance."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_bone_evaluation_probe import pmx
from run_geometry_probe import text


def target_pmx():
    source = pmx()
    names = text("登记骨3") + text("registration-bone-3")
    old = names + struct.pack("<3fbiH3f", 0, 2, 0, -1, 0, 0x3E, 0, 1, 0)
    new = names + struct.pack("<3fbiH3f", 0, 2, 0, -1, 0, 0x103E, 0, 1, 0)
    if source.count(old) != 1:
        raise ValueError("expected one complete target IK controller record")
    return source.replace(old, new)


def main():
    binary, root = [Path(arg).resolve() for arg in sys.argv[1:]]
    root.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture
    make_fixture(root, 255, [""] * 10)
    for name in ("data", "temp", "local"):
        (root / name).mkdir()
    child, target = root / "consumer.pmx", root / "target.pmx"
    child.write_bytes(pmx())
    target.write_bytes(target_pmx())
    exe = root / "probe.exe"
    shutil.copy2(binary, exe)
    env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(child), MMS_SCENE_TEST_TARGET_MODEL=str(target),
               TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    command = [str(exe), "--bone-scene-evaluation-only"]
    with (root / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
            creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    report = dict(command=command, exitCode=result.returncode,
        inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, exe, child, target)})
    (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print((root / "result.log").read_text(encoding="utf-8", errors="replace")[-8000:])
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
