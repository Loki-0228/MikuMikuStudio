"""Verify evaluate 1.3 parent states through a real hidden host and plugin."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from run_native_registration_probe import pmx


def main():
    binary, plugin, root = [Path(arg).resolve() for arg in sys.argv[1:]]
    root.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture
    make_fixture(root, 255, [""] * 10)
    for name in ("data", "temp", "local"):
        (root / name).mkdir()
    model = root / "parent-states.pmx"
    model.write_bytes(pmx())
    package = root / "PLUGINS" / "evaluation"
    package.mkdir(parents=True)
    shutil.copy2(plugin, package / "mms_evaluation_host_probe.dll")
    manifest = dict(schemaVersion=1, id="org.example.evaluation-host", name="Parent state probe", version="1.0.0",
        kind="native", abi=dict(major=1, minMinor=0), entry="mms_evaluation_host_probe.dll", architecture="x64",
        minWindows="6.1.7601", capabilities=["scene.evaluate"], requiresServices=[dict(id="mms.scene.evaluate", major=1, minMinor=3)],
        dependencies=[], nativeDependencies=[], reloadPolicy="restart")
    (package / "plugin.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    exe = root / "probe.exe"
    shutil.copy2(binary, exe)
    env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model), MMS_PLUGIN_TEST_DIR=str(root / "PLUGINS"),
               TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    command = [str(exe), "--evaluation-parent-state-only"]
    with (root / "result.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
            creationflags=subprocess.CREATE_NO_WINDOW, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    report = dict(command=command, exitCode=result.returncode,
        inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary, exe, plugin, model)})
    (root / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print((root / "result.log").read_text(encoding="utf-8", errors="replace")[-8000:])
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
