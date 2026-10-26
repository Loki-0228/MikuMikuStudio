"""Check owned bone definitions through real PMX/PMD loading in hidden hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_native_registration_probe import pmx, pmd


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
        model = root / ("bone-definition." + kind)
        model.write_bytes(make_model())
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
            TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--bone-definition-only"]
        with (root / "result.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
                creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        report["cases"].append(dict(kind=kind, command=command, exitCode=result.returncode,
            modelSha256=hashlib.sha256(model.read_bytes()).hexdigest()))
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(kind, (root / "result.log").read_text(encoding="utf-8", errors="replace")[-6000:], flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
