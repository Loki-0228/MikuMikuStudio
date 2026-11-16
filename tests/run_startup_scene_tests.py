"""Run the real startup/readout host in disposable application directories."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mme", type=Path)
    parser.add_argument("--project", type=Path,
                        help="Check repeated loading and stale viewport restoration with this PMM")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="startup-", dir=args.output.resolve()))
    cases = ["bare", "settings", "plugins"] + (["mme"] if args.mme else [])
    failed = False
    for case in cases:
        fixture = root / case
        fixture.mkdir()
        for source in args.runtime.glob("*.dll"):
            shutil.copy2(source, fixture / source.name)
        executable = fixture / "startup_scene_tests.exe"
        shutil.copy2(args.host, executable)
        if case != "bare":
            (fixture / "data").mkdir()
            for name in ("settings.json", "mmconfig.ini"):
                source = args.runtime / "data" / name
                if source.exists():
                    shutil.copy2(source, fixture / "data" / name)
        if case in ("plugins", "mme"):
            shutil.copytree(args.runtime / "PLUGINS", fixture / "PLUGINS")
        if case == "mme":
            legacy = fixture / "PLUGINS" / "LEGACY" / "mme"
            legacy.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.mme, legacy / "MMEffect.dll")
        env = os.environ.copy()
        env.pop("MMS_STARTUP_TEST_VISIBLE", None)
        env.pop("MMS_TEST_PROJECT", None)
        if args.project:
            env["MMS_TEST_PROJECT"] = str(args.project.resolve())
        env.update(LOCALAPPDATA=str(fixture / "localappdata"),
                   APPDATA=str(fixture / "appdata"),
                   MIKUMIKUSTUDIO_TRACE_FILE=str(fixture / "trace.log"))
        try:
            result = subprocess.run([str(executable)], cwd=fixture, env=env,
                                    capture_output=True, timeout=90)
            log = result.stdout + result.stderr
            passed = result.returncode == 0
        except subprocess.TimeoutExpired as error:
            log = (error.stdout or b"") + (error.stderr or b"") + b"\nFAIL timeout\n"
            passed = False
        (fixture / "result.log").write_bytes(log)
        print(f"{case}: {'PASS' if passed else 'FAIL'} {fixture / 'result.log'}", flush=True)
        if not passed:
            print(log.decode(errors="replace"), flush=True)
        failed |= not passed
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
