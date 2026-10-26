"""Verify private registered local bone preparation through real PMX/PMD loading."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_native_registration_probe import pmx as registration_pmx, pmd as registration_pmd
from run_geometry_probe import text

p = struct.pack


def pmx():
    source = registration_pmx()
    begin = source.index(p("<i", 4) + text("登记骨0"))
    end = source.index(p("<i", 4) + text("登记表情1"), begin)
    tip = text("fresh-tip") + text("fresh-tip") + p("<3fbiH3f", 0, 3, 0, -1, 0, 0x10, 0, 1, 0)
    return source[:begin] + p("<i", 5) + source[begin + 4:end] + tip + source[end:]


def pmd():
    source = registration_pmd()
    fixed = lambda value: value.encode("ascii").ljust(20, b"\0")
    begin = source.index(p("<H", 4) + fixed("bone-0"))
    end = begin + 2 + 4 * 39
    tip = fixed("fresh-tip") + p("<HHBH3f", 65535, 0, 7, 0, 0, 3, 0)
    corotate = fixed("fresh-corotate") + p("<HHBH3f", 65535, 0, 9, 50, 1, 3, 0)
    return source[:begin] + p("<H", 6) + source[begin + 2:end] + tip + corotate + source[end:]


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
        model = root / ("bone-kinematic." + kind)
        model.write_bytes(make_model())
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
                   TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--bone-kinematic-only"]
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
