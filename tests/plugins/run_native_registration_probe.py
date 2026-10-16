"""Exercise actual morph/display registration with a real edit DLL in isolated hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_geometry_probe import pmx as geometry_pmx, text

p = struct.pack


def pmx():
    # Keep the established mesh/material format, replacing only this script's
    # bone/morph/physics sections. No shared fixture is changed.
    source = geometry_pmx()
    begin = source.index(p("<i", 4) + text("骨骼0"))
    data = bytearray(source[:begin]) + p("<i", 4)
    for index, (parent, flags, position) in enumerate((
        (-1, 0x1E, (0, 0, 0)), (0, 0x1A, (0, 1, 0)),
        (1, 0x1A, (0, 2, 0)), (-1, 0x3E, (0, 2, 0)),
    )):
        data += text("登记骨" + str(index)) + text("registration-bone-" + str(index))
        data += p("<3fbiH3f", *position, parent, 0, flags, 0, 1, 0)
        if flags & 0x20:
            data += p("<bifibB", 2, 1, .5, 1, 1, 0)
    data += p("<i", 4)
    for panel in range(1, 5):
        data += text("登记表情" + str(panel)) + text("registration-morph-" + str(panel))
        data += p("<BBiB3f", panel, 1, 1, 0, panel * .125, 0, 0)
    # One facial display frame, no rigid bodies or joints. IK is genuinely
    # parsed from the file; it is not substituted after loading.
    data += p("<i", 1) + text("表情") + text("facials") + p("<Bi", 1, 4)
    for index in range(4):
        data += p("<Bb", 1, index)
    data += p("<ii", 0, 0)
    return data


def pmd():
    def fixed(value, size):
        return value.encode("ascii").ljust(size, b"\0")

    data = bytearray(b"Pmd" + p("<f", 1.) + fixed("Registration PMD", 20) + bytes(256))
    positions = [(-6, 4, 0), (-6, 16, 0), (6, 4, 0), (6, 16, 0)]
    data += p("<I", len(positions))
    for position in positions:
        data += p("<8fHHBB", *position, 0, 0, -1, 0, 0, 0, 0, 100, 0)
    data += p("<I6HI", 6, 0, 1, 2, 2, 1, 3, 1)
    data += p("<11fBBI", .8, .8, .8, 1, 1, 0, 0, 0, .4, .4, .4, 255, 0, 6) + fixed("color.bmp", 20)
    data += p("<H", 4)
    for index, (parent, tail, kind, position) in enumerate((
        (65535, 1, 1, (0, 0, 0)), (0, 2, 0, (0, 1, 0)),
        (1, 0, 6, (0, 2, 0)), (65535, 2, 2, (0, 2, 0)),
    )):
        data += fixed("bone-" + str(index), 20) + p("<HHBH3f", parent, tail, kind, 0, *position)
    data += p("<HHHBHfH", 1, 3, 2, 1, 1, .125, 1)
    data += p("<H", 5) + fixed("base", 20) + p("<IB", 4, 0)
    for index, position in enumerate(positions):
        data += p("<I3f", index, *position)
    for panel in range(1, 5):
        data += fixed("morph-" + str(panel), 20) + p("<IBI3f", 1, panel, 0, panel * .125, 0, 0)
    data += p("<B4H", 4, 1, 2, 3, 4)
    data += p("<BIB", 0, 0, 0) + bytes(1000) + p("<II", 0, 0)
    return data


def main():
    binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "pluginSha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "cases": []}
    for kind, filename in (("pmx", "MikuMikuDance.exe"), ("pmd", "自定义名称.exe")):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        model = root / ("registration." + kind)
        model.write_bytes(pmx() if kind == "pmx" else pmd())
        (root / "Data").mkdir()
        (root / "local").mkdir()
        package = root / "PLUGINS" / "编辑" / "登记测试"
        package.mkdir(parents=True)
        shutil.copy2(plugin, package / "manager_edit_probe.dll")
        manifest = dict(schemaVersion=1, id="org.example.editprobe", name="Registration history probe", version="1.0.0",
                        kind="native", abi=dict(major=1, minMinor=0), entry="manager_edit_probe.dll", architecture="x64",
                        minWindows="6.1.7601", capabilities=["ui.command", "ui.element"],
                        requiresServices=[dict(id="mms.ui", major=1, minMinor=0)], dependencies=[],
                        nativeDependencies=[], reloadPolicy="restart")
        (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
        shutil.copy2(binary, root / filename)
        env = os.environ.copy()
        env.update(LOCALAPPDATA=str(root / "local"), MMS_SCENE_TEST_MODEL=str(model))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(root / filename), "--native-registration-only"], cwd=root, env=env,
                                    startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace"))
        report["cases"].append({"kind": kind, "name": filename, "exitCode": result.returncode,
                                 "modelSha256": hashlib.sha256(model.read_bytes()).hexdigest()})
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    sys.exit(any(case["exitCode"] for case in report["cases"]))


if __name__ == "__main__":
    main()
