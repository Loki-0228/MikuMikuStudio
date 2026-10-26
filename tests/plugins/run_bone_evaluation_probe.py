"""Compare frozen bone evaluation with native hidden PMX/PMD pose passes."""
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_bone_kinematic_probe import pmx as local_pmx, pmd as local_pmd
from run_geometry_probe import text

p = struct.pack


def pmx():
    source = local_pmx()
    # Exercise a real nonempty after-physics sweep. Match the complete added
    # bone record so no unrelated flag/index byte can be changed accidentally.
    tip_names = text("fresh-tip") + text("fresh-tip")
    old_tip = tip_names + p("<3fbiH3f", 0, 3, 0, -1, 0, 0x10, 0, 1, 0)
    new_tip = tip_names + p("<3fbiH3f", 0, 3, 0, -1, 0, 0x1018, 0, 1, 0)
    if source.count(old_tip) != 1:
        raise ValueError("expected one complete fresh-tip bone record")
    source = source.replace(old_tip, new_tip)
    names = text("登记表情4") + text("registration-morph-4")
    old = names + p("<BBiB3f", 4, 1, 1, 0, .5, 0, 0)
    # Keep the existing four morph identities and facial-display references.
    # A genuine PMX bone morph affects the IK link, not merely mesh geometry.
    new = names + p("<BBib3f4f", 4, 2, 1, 1, .2, .1, 0,
                    0, 0, math.sin(.12), math.cos(.12))
    if source.count(old) != 1:
        raise ValueError("expected one registration morph to replace")
    return source.replace(old, new)


def pmd():
    source = bytearray(local_pmd())
    begin = source.index(p("<H", 6) + b"bone-0".ljust(20, b"\0"))
    # Native PMD preserves file bone types. Type 4 resets the chain's IK
    # accumulator, avoiding an accidental dependence on a previous pose.
    source[begin + 2 + 39 + 24] = 4
    return bytes(source)


def main():
    binary, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture
    report = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), cases=[])
    for kind, make_model in (("pmx", pmx), ("pmd", pmd)):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        for directory in ("data", "temp", "local"):
            (root / directory).mkdir()
        model = root / ("bone-evaluation." + kind)
        model.write_bytes(make_model())
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
                   TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--bone-evaluation-only"]
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
                creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        report["cases"].append(dict(kind=kind, command=command, exitCode=result.returncode,
            exeSha256=hashlib.sha256(exe.read_bytes()).hexdigest(), modelSha256=hashlib.sha256(model.read_bytes()).hexdigest()))
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace")[-6000:], flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
