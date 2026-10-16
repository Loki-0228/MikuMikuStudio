"""Generate original PMX/PMD/X fixtures and exercise the real scene ABI."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

p = struct.pack
def text(value):
    encoded = value.encode("utf-16-le")
    return p("<i", len(encoded)) + encoded

def pmx():
    # MMS's current file importer accepts PMX 2.0. The QDEF ABI record is
    # exercised separately by the C++ probe, not mislabeled as 2.1 file support.
    data = bytearray(b"PMX " + p("<f", 2.0) + bytes([8, 0, 1, 1, 1, 1, 1, 1, 1]))
    for name in ("接口测试", "ABI geometry", "", ""):
        data += text(name)
    data += p("<i", 5)
    for kind in range(5):
        data += p("<12fB", kind, kind + 4, 0, 0, 0, -1, .25, .75, 1, 2, 3, 4, min(kind, 2) if kind == 4 else kind)
        if kind == 0:
            data += p("<b", 0)
        elif kind in (1, 3):
            data += p("<2bf", 0, 1, .25)
            if kind == 3:
                data += p("<9f", 1, 2, 3, 4, 5, 6, 8, 9, 10)
        else:
            data += p("<4b4f", 0, 1, 2, 3, .1, .2, .3, .4)
        data += p("<f", .5)
    data += p("<i6Bi", 6, 0, 1, 2, 2, 3, 4, 1) + text("color.bmp")
    data += p("<i", 1) + text("材质") + text("material")
    data += p("<11fB5fbbBBB", .8, .7, .6, 1, .1, .2, .3, 8, .4, .3, .2,
              15, 0, 0, 0, 1, .5, 0, -1, 0, 1, 0)
    data += text("") + p("<i", 6)
    data += p("<i", 4)
    for i in range(4):
        data += text("骨骼" + str(i)) + text("bone" + str(i))
        data += p("<3fbiH3f", 0, i, 0, i - 1, 0, 26, 0, 1, 0)
    data += p("<i", 6)
    data += text("vertex") + text("") + p("<BBiB3f", 4, 1, 1, 0, 1, 0, 0)
    data += text("uv") + text("") + p("<BBiB4f", 4, 3, 1, 0, .1, .2, 0, 0)
    data += text("uv1") + text("") + p("<BBiB4f", 4, 4, 1, 0, .1, .2, .3, .4)
    data += text("material") + text("") + p("<BBibB28f", 4, 8, 1, 0, 1, *([.1] * 28))
    data += text("bone") + text("") + p("<BBib7f", 4, 2, 1, 1, 1, 2, 3, 0, 0, 0, 1)
    data += text("group") + text("") + p("<BBibf", 4, 0, 1, 0, .5)
    data += p("<ii", 0, 3)
    for i in range(3):
        data += text("rigid" + str(i)) + text("")
        data += p("<bBHB14fB", i, i, 0xffff, i, .5, 1, 1.5,
                  i, i + 4, 0, .1 * i, .2 * i, .3 * i, 2, .1, .2, .3, .4, i)
    data += p("<i", 1) + text("joint") + text("") + p("<Bbb24f", 0, 0, 1,
        .5, 4.5, 0, .05, .1, .15, -1, -2, -3, 1, 2, 3, -.1, -.2, -.3, .1, .2, .3, 2, 3, 4, 5, 6, 7)
    return data

def pmd():
    fixed = lambda value, size: value.encode("ascii").ljust(size, b"\0")
    data = bytearray(b"Pmd" + p("<f", 1.) + fixed("ABI PMD", 20) + bytes(256))
    positions = [(-6, 4, 0), (-6, 16, 0), (6, 4, 0), (6, 16, 0)]
    data += p("<I", 4)
    for pos in positions:
        data += p("<8fHHBB", *pos, 0, 0, -1, 0, 0, 0, 0, 100, 0)
    data += p("<I6HI", 6, 0, 1, 2, 2, 1, 3, 1)
    data += p("<11fBBI", .8, .8, .8, 1, 1, 0, 0, 0, .4, .4, .4, 255, 0, 6) + fixed("color.bmp", 20)
    data += p("<H", 1) + fixed("root", 20) + p("<HHBH3f", 65535, 0, 0, 0, 0, 0, 0)
    data += p("<HH", 0, 2)
    data += fixed("base", 20) + p("<IB", 4, 0)
    for i, pos in enumerate(positions):
        data += p("<I3f", i, *pos)
    data += fixed("smile", 20) + p("<IBI3f", 1, 4, 0, 1, 2, 3)
    data += p("<BH BIB", 1, 1, 0, 0, 0) + bytes(1000) + p("<II", 0, 0)
    return data

def accessory():
    return """xof 0303txt 0032
Mesh triangle {
3;
0.0;0.0;0.0;,1.0;0.0;0.0;,0.0;1.0;0.0;;
1;3;0,1,2;;
MeshNormals {3;0.0;0.0;-1.0;,0.0;0.0;-1.0;,0.0;0.0;-1.0;;1;3;0,1,2;;}
MeshTextureCoords {3;0.0;0.0;,1.0;0.0;,0.0;1.0;;}
MeshMaterialList {1;1;0;;Material {1.0;1.0;1.0;1.0;;8.0;0.2;0.2;0.2;;0.0;0.0;0.0;;TextureFilename {"color.bmp";}}}
}
"""

binary, output = [Path(value).resolve() for value in sys.argv[1:]]
output.mkdir(exist_ok=False)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pmd_selection_fixtures import make_fixture
report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "cases": []}
for kind in ("pmx", "pmd"):
    folder = output / kind
    make_fixture(folder, 255, [""] * 10)
    (folder / "fixture.pmd").write_bytes(pmd())
    (folder / "model.pmx").write_bytes(pmx())
    (folder / "triangle.x").write_text(accessory(), encoding="ascii")
    (folder / "Data").mkdir()
    (folder / "local").mkdir()
    shutil.copy2(binary, folder / "probe.exe")
    model = folder / ("model.pmx" if kind == "pmx" else "fixture.pmd")
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(folder / "local"), MMS_SCENE_TEST_MODEL=str(model),
               MMS_SCENE_TEST_ACCESSORY=str(folder / "triangle.x"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    command = [str(folder / "probe.exe"), "--geometry-only"]
    with (folder / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=folder, env=env, startupinfo=startup,
                                stdout=log, stderr=subprocess.STDOUT, timeout=120)
    report["cases"].append({"kind": kind, "command": command, "exitCode": result.returncode,
                             "modelSha256": hashlib.sha256(model.read_bytes()).hexdigest()})
    print(kind, (folder / "result.log").read_text(encoding="utf-8", errors="replace"))
(output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
sys.exit(any(case["exitCode"] for case in report["cases"]))
