"""Run the hidden project-open fixture with its own executable/data root."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def main():
    binary, output = (Path(value).resolve() for value in sys.argv[1:])
    scratch = output / f"{time.time_ns()}-{os.getpid()}"
    scratch.mkdir(parents=True, exist_ok=False)
    staged = scratch / binary.name
    shutil.copy2(binary, staged)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    log = scratch / "result.log"
    error = None
    code = 1
    with log.open("w", encoding="utf-8") as stream:
        try:
            result = subprocess.run([str(staged)], cwd=scratch, stdin=subprocess.DEVNULL,
                                    stdout=stream, stderr=subprocess.STDOUT,
                                    startupinfo=startup, timeout=120)
            code = result.returncode
        except (OSError, subprocess.TimeoutExpired) as exception:
            error = str(exception)
    report = {"binary": str(binary), "sha256": hashlib.sha256(staged.read_bytes()).hexdigest(),
              "cwd": str(scratch), "exitCode": code, "runnerError": error}
    (scratch / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(log.read_text(encoding="utf-8", errors="replace"))
    print(json.dumps(report))
    return int(code != 0 or error is not None)


if __name__ == "__main__":
    sys.exit(main())
