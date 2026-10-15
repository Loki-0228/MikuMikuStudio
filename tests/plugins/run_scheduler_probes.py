"""Run real MMS frame paths only in fresh hidden, isolated test directories.

Usage: python run_scheduler_probes.py BUILD_DIR OUTPUT_DIR MEDIA_FIXTURES
MEDIA_FIXTURES contains corners.mp4 and alpha-triangle.pmx from the existing
native media fixture suite. No installed or already-running editor is used.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

build, output, media = map(lambda value: Path(value).resolve(), sys.argv[1:])
output.mkdir(exist_ok=False)
report = []
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0


def run(name, executable, args=(), fixtures=(), coordinator=False, model=False):
    folder = output / name
    folder.mkdir()
    (folder / "Data").mkdir()
    (folder / "local").mkdir()
    source = build / executable
    target = folder / "probe.exe"
    shutil.copy2(source, target)
    inputs = {str(source): hashlib.sha256(source.read_bytes()).hexdigest()}
    for filename in fixtures:
        source_file = media / filename
        shutil.copy2(source_file, folder / filename)
        inputs[str(source_file)] = hashlib.sha256(source_file.read_bytes()).hexdigest()
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(folder / "local")
    if coordinator:
        env["MMS_TEST_COORDINATOR"] = "1"
    if model:
        env["MMS_DEVICE_TEST_MODEL"] = str(folder / "alpha-triangle.pmx")
        env["MMS_MENU_SEQUENCE_MODEL"] = str(folder / "alpha-triangle.pmx")
    if name == "image-sequence" and env.get("MMS_AA_SEQUENCE_MODEL"):
        aa_model = Path(env["MMS_AA_SEQUENCE_MODEL"]).resolve()
        inputs[str(aa_model)] = hashlib.sha256(aa_model.read_bytes()).hexdigest()
    command = [str(target), *(str(folder) if arg == "{folder}" else arg for arg in args)]
    with (folder / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=folder, env=env, startupinfo=startup,
                                stdout=log, stderr=subprocess.STDOUT, timeout=120)
    report.append({"name": name, "command": command, "coordinator": coordinator,
                   "model": env.get("MMS_DEVICE_TEST_MODEL"),
                   "aaModel": env.get("MMS_AA_SEQUENCE_MODEL") if name == "image-sequence" else None, "sha256": inputs,
                   "exitCode": result.returncode})
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(name, "exit", result.returncode, flush=True)
    if result.returncode:
        print((folder / "result.log").read_text(encoding="utf-8", errors="replace")[-4500:], flush=True)
        raise SystemExit(result.returncode)


run("scheduler", "tests/sidebar/Release/sidebar_resize_tests.exe", ["--scheduler-only"])
run("sidebar", "tests/sidebar/Release/sidebar_resize_tests.exe")
for entry, cases in [("frame", ["ok", "lost-ready", "lost-reset", "reenter", "direct-reset-fail", "paused-background-seek"]),
                     ("record-frame", ["ok", "lost-ready", "lost-reset", "reenter", "reset-fail", "cancel"])]:
    for case in cases:
        run(entry + "-" + case, "tests/device-lost/Release/device_lost_wait_tests.exe",
            ["--entry", entry, "--case", case, "--mode", "windowed", "--coordinator", "1"],
            ["alpha-triangle.pmx"], coordinator=True, model=True)
run("stopped-media", "tests/media/Release/MikuMikuDance.exe", ["{folder}", "--stopped-coordinator"], ["corners.mp4"], coordinator=True)
run("image-sequence", "tests/Release/image_sequence_tests.exe", ["{folder}", "--hidden-production"],
    ["alpha-triangle.pmx"], coordinator=True, model=True)
