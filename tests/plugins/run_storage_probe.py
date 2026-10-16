"""Exercise the real serializer DLL and PMM writer/loader in isolated hosts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

binary, plugin, model, output = [Path(arg).resolve() for arg in sys.argv[1:]]
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
output.mkdir(exist_ok=False)
report = {"inputs": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (binary, plugin, model)}, "runs": []}
for index, name in enumerate(("MikuMikuDance.exe", "自定义名称.exe")):
    root = output / f"run-{index}"
    root.mkdir()
    (root / "Data").mkdir()
    (root / "local").mkdir()
    package = root / "PLUGINS" / "原创测试" / "状态保存"
    package.mkdir(parents=True)
    shutil.copy2(binary, root / name)
    shutil.copy2(plugin, package / "lifecycle.dll")
    shutil.copy2(model, root / "model.pmx")
    manifest = {"schemaVersion": 1, "id": "org.example.gui-storage", "name": "工程保存探针", "version": "1.0.0",
                "kind": "native", "abi": {"major": 1, "minMinor": 0}, "entry": "lifecycle.dll", "architecture": "x64",
                "minWindows": "6.1.7601", "capabilities": ["ui.command", "storage.serializer"],
                "requiresServices": [{"id": "mms.storage", "major": 1, "minMinor": 0}],
                "dependencies": [], "nativeDependencies": [], "reloadPolicy": "restart"}
    (package / "plugin.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(root / "local"), MMS_SCENE_TEST_MODEL=str(root / "model.pmx"), MMS_ABI_PROBE_LOG=str(root / "plugin.log"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    with (root / "result.log").open("w", encoding="utf-8") as log:
        process = subprocess.run([str(root / name), "--storage-only"], cwd=root, env=env, startupinfo=startup,
                                 stdout=log, stderr=subprocess.STDOUT, timeout=120)
    print((root / "result.log").read_text(encoding="utf-8", errors="replace"))
    lines = (root / "plugin.log").read_text(encoding="utf-8").splitlines() if (root / "plugin.log").exists() else []
    checks = {"native": process.returncode == 0, "opened": lines.count("PROJECT_OPENED_PASS") == 1,
              "saved": lines.count("PROJECT_SAVED_PASS") == 1, "scene_at_event": lines.count("SCENE_READ_PASS") == 2,
              "released": lines.count("SERIALIZER_SAVE") == lines.count("SERIALIZER_RELEASE"),
              "no_callback_failure": not any("FAIL" in line for line in lines), "unloaded": lines.count("DESTROY") == 1}
    report["runs"].append({"name": name, "exitCode": process.returncode, "checks": checks})
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    if not all(checks.values()):
        print(json.dumps(checks, indent=2)); print("\n".join(lines)); sys.exit(1)
print("Real plugin PMM round trip and EXE rename PASS")
