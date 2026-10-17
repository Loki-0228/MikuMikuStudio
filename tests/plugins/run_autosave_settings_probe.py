"""Exercise real processes sharing one profile but using independent EXE/Data folders."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, output = [Path(arg).resolve() for arg in sys.argv[1:]]
output.mkdir(exist_ok=False)
work = output / "unrelated-cwd"
work.mkdir()
profile = output / "shared-profile"
legacy = profile / "MikuDanceStudio" / "autosave.ini"
legacy.parent.mkdir(parents=True)
legacy.write_text("[autosave]\nenabled=1\nintervalMinutes=9\nmaxBackups=3\ndirectory=" + str(output / "old-install" / "backups") + "\n", encoding="utf-16")
legacy_bytes = legacy.read_bytes()
legacy_aa = legacy.with_name("antialias.ini")
legacy_aa.write_text("[Antialias]\nAlgorithm=5\nEnabled=0\n", encoding="utf-16")
legacy_aa_bytes = legacy_aa.read_bytes()
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
results = []

def install(name):
    folder = output / name
    folder.mkdir()
    executable = folder / "MikuMikuDance.exe"
    shutil.copy2(binary, executable)
    return folder, executable

def run(executable, folder, mode, *, override=False, no_profile=False):
    env = os.environ.copy()
    env.pop("MIKUDANCESTUDIO_AUTOSAVE_CONFIG", None)
    env["LOCALAPPDATA"] = str(profile)
    if no_profile:
        env.pop("LOCALAPPDATA", None)
    if override:
        env["MIKUDANCESTUDIO_AUTOSAVE_CONFIG"] = str(folder / "override.ini")
    result = subprocess.run([str(executable), str(folder), mode], cwd=work, env=env,
                            startupinfo=startup, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    log = result.stdout.decode("utf-8", errors="replace")
    results.append(dict(executable=str(executable), mode=mode, exitCode=result.returncode, log=log))
    (output / f"case-{len(results):02}.log").write_text(log, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(log)

a, exe_a = install("stable")
b, exe_b = install("abi")
run(exe_a, a, "--settings-fresh")
run(exe_b, b, "--settings-fresh")
run(exe_a, a, "--settings-custom")
run(exe_b, b, "--settings-default")
renamed = a / "\u81ea\u5b9a\u4e49\u540d\u79f0.exe"
exe_a.rename(renamed)
run(renamed, a, "--settings-check-custom")
run(renamed, a, "--settings-readonly")
c, exe_c = install("copied-default")
shutil.copytree(b / "data", c / "data")
run(exe_c, c, "--settings-default")
d, exe_d = install("explicit-override")
run(exe_d, d, "--settings-override", override=True)
e, exe_e = install("no-system-profile")
run(exe_e, e, "--settings-fresh", no_profile=True)
assert legacy_aa.read_bytes() == legacy_aa_bytes, "shared antialias preferences must remain untouched"
assert legacy.read_bytes() == legacy_bytes, "legacy global settings must remain untouched"
assert not (output / "old-install").exists(), "stale backup address must never be created"
assert not list((a / "data").glob("*.tmp")), "failed atomic replacement must remove its temporary file"
assert not (work / "data").exists(), "settings must not follow process cwd"
report = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), cases=results,
              legacyUnchanged=True, staleDirectoryNotCreated=True, cwdIndependent=True)
(output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"PASS {len(results)} real-process settings cases: independent installations, renamed EXE, relocated default, override and missing system profile")
