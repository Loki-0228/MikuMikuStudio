"""Load original PMX/PMD fixtures and verify owned bind meshes in hidden hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_geometry_probe import pmx as geometry_pmx, pmd as geometry_pmd, text

p = struct.pack


def pmx():
    source = bytes(geometry_pmx())
    fields = p("<11fB5fbbBBB", .8, .7, .6, 1, .1, .2, .3, 8, .4, .3, .2,
               15, 0, 0, 0, 1, .5, 0, -1, 0, 1, 0)
    old = p("<i", 1) + text("材质") + text("material") + fields + text("") + p("<i", 6)
    new = p("<i", 2)
    for index in range(2):
        new += text("材质" + str(index)) + text("material-" + str(index)) + fields + text("") + p("<i", 3)
    if source.count(old) != 1:
        raise ValueError("expected one complete original PMX material block")
    # Keep the real SDEF record and PMX 2.0 version from the original fixture.
    return source.replace(old, new)


def pmd():
    source = bytes(geometry_pmd())
    positions = ((-6, 4, 0), (-6, 16, 0), (6, 4, 0), (6, 16, 0))
    for index, position in enumerate(positions):
        old = p("<8fHHBB", *position, 0, 0, -1, 0, 0, 0, 0, 100, 0)
        # 197 becomes the native signed int8 value -59. This verifies the
        # loader's retained byte and signed percentage path without clamping.
        new = p("<8fHHBB", *position, 0, 0, -1, 0, 0, 0, 0,
                (0, 25, 100, 197)[index], (0, 1, 0, 197)[index])
        if source.count(old) != 1:
            raise ValueError("expected one complete original PMD vertex record")
        source = source.replace(old, new)
    fields = p("<11fBBI", .8, .8, .8, 1, 1, 0, 0, 0, .4, .4, .4, 255, 0, 6)
    texture = b"color.bmp".ljust(20, b"\0")
    old = p("<I6HI", 6, 0, 1, 2, 2, 1, 3, 1) + fields + texture
    segment = p("<11fBBI", .8, .8, .8, 1, 1, 0, 0, 0, .4, .4, .4, 255, 0, 3) + texture
    new = p("<I6HI", 6, 0, 1, 2, 2, 1, 3, 2) + segment + segment
    if source.count(old) != 1:
        raise ValueError("expected one complete original PMD face/material block")
    return source.replace(old, new)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    binary, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    report = dict(binary=str(binary), sha256=digest(binary), cases=[],
                  scope="PMX 2.0 and PMD loaded meshes; QDEF/unknown are runtime-only rejection injections",
                  additionalUvCoverage="one loaded additional UV group; UV groups 2-4 are not covered by this host fixture")
    failed = False
    for kind, make_model in (("pmx", pmx), ("pmd", pmd)):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        for directory in ("data", "temp", "local"):
            (root / directory).mkdir()
        model = root / ("mesh-definition." + kind)
        model.write_bytes(make_model())
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
                   TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--mesh-definition-only"]
        case = dict(kind=kind, command=command,
                    inputs={str(path): digest(path) for path in (exe, model, root / "color.bmp")})
        log_path = root / "result.log"
        with log_path.open("w", encoding="utf-8") as log:
            try:
                result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
                                        creationflags=subprocess.CREATE_NO_WINDOW, stdin=subprocess.DEVNULL,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=240)
                case["exitCode"] = result.returncode
            except subprocess.TimeoutExpired:
                case.update(exitCode=124, error="Host probe exceeded 240 seconds")
                log.write("\nFAIL: Host probe exceeded 240 seconds\n")
            except OSError as error:
                case.update(exitCode=125, error=str(error))
                log.write("\nFAIL: " + str(error) + "\n")
        case["logSha256"] = digest(log_path)
        project = root / "mesh-definition.pmm"
        if project.exists():
            case["projectSha256"] = digest(project)
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        case["completionMarker"] = "Frozen mesh definitions: real BIND geometry" in log_text
        report["cases"].append(case)
        (root / "report.json").write_text(json.dumps(case, indent=2), encoding="utf-8")
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(kind, log_text[-12000:], flush=True)
        failed = bool(case["exitCode"]) or not case["completionMarker"] or failed
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
