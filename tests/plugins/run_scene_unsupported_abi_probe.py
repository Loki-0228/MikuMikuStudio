"""Probe unsupported morph rejection through an isolated real host and DLL."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_bone_evaluation_probe import pmx, pmd


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    binary, plugin, output = [Path(value).resolve() for value in sys.argv[1:]]
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    report = dict(inputs={str(path): digest(path) for path in (binary, plugin)}, cases=[])
    failed = False
    for kind, make_model in (("pmx", pmx), ("pmd", pmd)):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        for name in ("data", "temp", "local"):
            (root / name).mkdir()
        model = root / ("scene-unsupported." + kind)
        model.write_bytes(make_model())
        package = root / "PLUGINS" / "evaluation"
        package.mkdir(parents=True)
        dll = package / "mms_evaluation_host_probe.dll"
        shutil.copy2(plugin, dll)
        manifest = dict(schemaVersion=1, id="org.example.evaluation-host", name="Unsupported scene morph probe",
            version="1.0.0", kind="native", abi=dict(major=1, minMinor=0), entry=dll.name,
            architecture="x64", minWindows="6.1.7601", capabilities=["scene.evaluate"],
            requiresServices=[dict(id="mms.scene.evaluate", major=1, minMinor=4)],
            dependencies=[], nativeDependencies=[], reloadPolicy="restart")
        manifest_path = package / "plugin.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model), MMS_PLUGIN_TEST_DIR=str(root / "PLUGINS"),
            TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--scene-unsupported-abi-only"]
        case = dict(kind=kind, command=command,
            inputs={str(path): digest(path) for path in (exe, dll, model, manifest_path)})
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
        report["cases"].append(case)
        (root / "report.json").write_text(json.dumps(case, indent=2), encoding="utf-8")
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(kind, log_path.read_text(encoding="utf-8", errors="replace")[-10000:], flush=True)
        failed = bool(case["exitCode"]) or failed
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
