"""Opt-in integration using locally supplied original MME DLLs, never shipped.

Run from an interactive Windows desktop. Each case has its own installation,
data directory, original DLL copies, and deliberately renamed test executable.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def isolated_environment(directory):
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("MMS_")}
    for name in ("temp", "local"):
        (directory / name).mkdir()
    env.update(TEMP=str(directory / "temp"), TMP=str(directory / "temp"),
               LOCALAPPDATA=str(directory / "local"))
    return env

X_MESH = """xof 0303txt 0032
Mesh {
3;
-1.0;0.0;0.0;,
0.0;2.0;0.0;,
1.0;0.0;0.0;;
1;
3;0,1,2;;
MeshMaterialList {
1;1;0;;
Material { 1.0;0.0;0.0;1.0;; 1.0; 0.0;0.0;0.0;; 0.0;0.0;0.0;; }
}
}
"""
FX = """float4x4 WVP : WORLDVIEWPROJECTION;
struct Input { float4 position : POSITION0; };
struct Output { float4 position : POSITION0; };
Output VS(Input input) { Output o; o.position = mul(input.position, WVP); return o; }
float4 PS() : COLOR0 { return float4(0,1,0,1); }
technique Object < string MMDPass = "object"; > {
pass P { CullMode = NONE; AlphaBlendEnable = FALSE;
VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); }
}
"""

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--originals", type=Path, required=True)
    parser.add_argument("--host", type=Path, default=ROOT / "build/cmake/tests/mme/Release/MikuMikuDance.exe")
    parser.add_argument("--adapter", type=Path, default=ROOT / "build/cmake/Release/MMSAdapter.dll")
    parser.add_argument("--ascii-only", action="store_true")
    parser.add_argument("--bridge", type=Path, help="Use the built-in MMHack bridge instead of the original hack")
    args = parser.parse_args()
    originals = {name: args.originals.resolve() / name for name in ("MMHack.dll", "MMEffect.dll")}
    before = {name: digest(path) for name, path in originals.items()}
    root = Path(tempfile.mkdtemp(prefix="legacy-mme-", dir=ROOT / "build/verification"))
    report = {"root": str(root), "originals": before, "cases": []}
    names = ["An arbitrarily named MMS.exe"]
    if not args.ascii_only:
        names.append("自定义名字的MMS.exe")
    for index, name in enumerate(names):
        directory = root / str(index)
        directory.mkdir()
        exe = directory / name
        shutil.copy2(args.host, exe)
        shutil.copy2(args.adapter, directory / "MMSAdapter.dll")
        if args.bridge:
            shutil.copy2(args.bridge, directory / "MMHack.dll")
        package = directory / "PLUGINS/effects/user/legacy_mme"
        package.mkdir(parents=True)
        for filename, path in originals.items():
            shutil.copy2(path, package / filename)
        (package / "MMEffect.ini").write_text("[System]\nSkipValidation=true\nEMMAutoSave=false\n", encoding="ascii")
        manifest = dict(schemaVersion=1, id="user.legacy.mme", name="Local MME", version="0.37.0",
                        kind="legacy_mme", entry="MMEffect.dll", hack="MMHack.dll",
                        architecture="x64", minWindows="6.1.0", reloadPolicy="restart")
        if args.bridge:
            manifest.pop("hack")
        (package / "plugin.json").write_text(json.dumps(manifest), encoding="utf-8")
        (directory / "PLUGINS/config.json").write_text(json.dumps(dict(schemaVersion=1, maxDepth=3, disabledPlugins=[])), encoding="utf-8")
        model = directory / "probe.x"
        model.write_text(X_MESH, encoding="ascii")
        model.with_suffix(".fx").write_text(FX, encoding="ascii")
        project = directory / "relative-project"
        (project / "effects").mkdir(parents=True)
        (project / "scene.emm").write_text("[Effect]\nPmd1=effects/relative.fx\n", encoding="ascii")
        (project / "effects/relative.fx").write_text("project-relative-effect", encoding="ascii")
        case = dict(executable=name, hostHash=digest(exe), adapterHash=digest(directory / "MMSAdapter.dll"))
        environment = isolated_environment(directory)
        with (directory / "result.log").open("wb") as out:
            try:
                run = subprocess.run([str(exe), str(model), "1", "--legacy-package"], cwd=directory,
                                     env=environment, stdin=subprocess.DEVNULL,
                                     stdout=out, stderr=subprocess.STDOUT, timeout=90,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
                case["exitCode"] = run.returncode
            except subprocess.TimeoutExpired:
                case.update(exitCode=None, error="timeout")
        log = (directory / "result.log").read_text(encoding="utf-8", errors="replace")
        case["sourceCopiesUnchanged"] = all(digest(package / n) == h for n, h in before.items())
        case["settingsRemainInData"] = not any(directory.glob("MMEffect.*"))
        case["passed"] = case.get("exitCode") == 0 and "PASS renamed packaged MME" in log and case["sourceCopiesUnchanged"] and case["settingsRemainInData"]
        case["cacheDlls"] = [str(p.relative_to(directory)) for p in (directory / "data").rglob("*.dll")]
        stable = list((directory / "data/plugins/legacy_mme/settings").glob("*/MMEffect.ini"))
        case["stableSettings"] = len(stable) == 1 and "wide=retained" in stable[0].read_text(encoding="utf-8", errors="replace")
        case["passed"] = case["passed"] and case["stableSettings"]
        if case["passed"]:
            # Change only this isolated installation's original adapter copy.
            # A PE overlay changes the cache key while retaining exported code.
            with (directory / "MMSAdapter.dll").open("ab") as adapter:
                adapter.write(b"MMS settings upgrade probe")
            with (directory / "upgrade.log").open("wb") as out:
                upgrade = subprocess.run([str(exe), str(model), "1", "--legacy-package"], cwd=directory,
                    env=environment, stdin=subprocess.DEVNULL, stdout=out,
                    stderr=subprocess.STDOUT, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
            case["upgradeExitCode"] = upgrade.returncode
            generations = [p for p in (directory / "data/plugins/legacy_mme").iterdir() if p.is_dir() and len(p.name) == 64]
            case["upgradeRetainedSettings"] = upgrade.returncode == 0 and len(generations) == 2 and len(stable) == 1 and \
                stable[0].read_text(encoding="utf-8", errors="replace").count("wide=retained") >= 2 and \
                all(not (p / "MMEffect.ini").exists() for p in generations)
            case["passed"] = case["passed"] and case["upgradeRetainedSettings"]
        if case["passed"] and index == 0:
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p]
            kernel.CreateFileW.restype = ctypes.c_void_p
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            lock = kernel.CreateFileW(str(stable[0].parent / "preferences.lock"), 0x80000000, 0, None, 3, 0x80, None)
            if lock in (None, ctypes.c_void_p(-1).value):
                raise RuntimeError("cannot hold isolated preferences lock")
            try:
                held = subprocess.run([str(exe), str(model), "1", "--legacy-package"], cwd=directory,
                    env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
            finally:
                kernel.CloseHandle(lock)
            (directory / "locked.log").write_bytes(held.stdout + held.stderr)
            case["lockedPreferencesRefused"] = held.returncode == 7 and b"legacy file access error 32" in held.stdout
            backup = stable[0].with_name("preserved-settings.ini")
            stable[0].rename(backup)
            old = generations[0] / "MMEffect.ini"
            old.write_bytes(b"[System]\nSkipValidation=true\n;unattributed-old-preferences\n")
            try:
                conflict = subprocess.run([str(exe), str(model), "1", "--legacy-package"], cwd=directory,
                    env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
                (directory / "conflict.log").write_bytes(conflict.stdout + conflict.stderr)
                case["unattributedPreferencesRefused"] = conflict.returncode == 7 and \
                    b"unattributed old MMEffect.ini" in conflict.stdout and not stable[0].exists() and \
                    b"unattributed-old-preferences" in old.read_bytes()
            finally:
                old.unlink()
                backup.rename(stable[0])
            stable[0].rename(backup)
            sentinel = directory / "hardlink-sentinel.ini"
            sentinel.write_bytes(b"[System]\n;must-not-change\n")
            os.link(sentinel, stable[0])
            try:
                linked = subprocess.run([str(exe), str(model), "1", "--legacy-package"], cwd=directory,
                    env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
                (directory / "hardlink.log").write_bytes(linked.stdout + linked.stderr)
                case["hardlinkedPreferencesRefused"] = linked.returncode == 7 and \
                    b"invalid legacy preferences file" in linked.stdout and sentinel.read_bytes() == b"[System]\n;must-not-change\n"
            finally:
                stable[0].unlink()
                backup.rename(stable[0])
            case["passed"] = case["passed"] and case["lockedPreferencesRefused"] and case["unattributedPreferencesRefused"] and case["hardlinkedPreferencesRefused"]
        report["cases"].append(case)
        print(name, case["passed"], case.get("exitCode"), flush=True)
        if not case["passed"]:
            print(log[-3000:], flush=True)
            break
    report["originalsUnchanged"] = all(digest(p) == before[n] for n, p in originals.items())
    report["passed"] = report["originalsUnchanged"] and len(report["cases"]) == len(names) and all(c["passed"] for c in report["cases"])
    (root / "summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(root / "summary.json", flush=True)
    return 0 if report["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
