"""Check frozen morph definitions with original PMX/PMD fixtures in hidden hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_geometry_probe import text
from run_native_registration_probe import pmd as registration_pmd

p = struct.pack


def pmx():
    # Four additional UV sets let the actual loader produce all five restore
    # families. Every PMX 2.0 morph type has nonempty original payloads.
    data = bytearray(b"PMX " + p("<f", 2.0) + bytes([8, 0, 4, 1, 1, 1, 1, 1, 1]))
    for name in ("Morph definitions", "Morph definitions", "", ""):
        data += text(name)
    data += p("<i", 3)
    for index, position in enumerate(((-1, 1, 0), (1, 1, 0), (0, 3, 0))):
        data += p("<24f", *position, 0, 0, -1, .25, .75,
                  *[index + family + component * .125 for family in range(4) for component in range(4)])
        data += p("<Bbf", 0, 0, 1.)
    data += p("<i3Bi", 3, 0, 1, 2, 1) + text("color.bmp")
    data += p("<i", 1) + text("material") + text("material")
    data += p("<11fB5fbbBBB", .8, .7, .6, 1, .1, .2, .3, 8, .4, .3, .2,
              15, 0, 0, 0, 1, .5, 0, -1, 0, 1, 0)
    data += text("") + p("<i", 3)
    data += p("<i", 1) + text("root") + text("root") + p("<3fbiH3f", 0, 0, 0, -1, 0, 0x1E, 0, 1, 0)
    data += p("<i", 9)
    for kind in range(9):
        data += text("morph-" + str(kind)) + text("definition-" + str(kind))
        data += p("<BBi", 4, kind, 2 if kind == 0 else 1)
        if kind == 0:
            data += p("<bfbf", 1, .25, 1, -.5)  # Preserve duplicate group entries and negative weight.
        elif kind == 1:
            data += p("<B3f", 2, .25, -.5, .75)
        elif kind == 2:
            data += p("<b7f", 0, .5, 1, -.25, 0, 0, 0, 1)
        elif kind <= 7:
            data += p("<B4f", 1, kind * .1, -.2, .3, -.4)
        else:
            data += p("<bB28f", -1, 197, *[.01 * (index + 1) for index in range(28)])
    data += p("<iii", 0, 0, 0)  # Display frames, rigid bodies, joints.
    return data


def pmd():
    data = bytearray(registration_pmd())
    # Reorder the base table so file-local offset index zero becomes vertex 3.
    # The native loader must remap later morphs before the snapshot sees them.
    begin = data.index(b"base".ljust(20, b"\0")) + 25
    entries = [bytes(data[begin + index * 16:begin + (index + 1) * 16]) for index in range(4)]
    data[begin:begin + 64] = b"".join(entries[index] for index in (3, 1, 2, 0))
    return data


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
        model = root / ("morph-definition." + kind)
        model.write_bytes(make_model())
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
                   TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--morph-definition-only"]
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
                creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        report["cases"].append(dict(kind=kind, command=command, exitCode=result.returncode,
            exeSha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
            modelSha256=hashlib.sha256(model.read_bytes()).hexdigest()))
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace")[-6000:], flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
