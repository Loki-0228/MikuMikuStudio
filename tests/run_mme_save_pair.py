"""Opt-in x64 Windows PMM/EMM regression using a local, genuine MME 0.37."""
import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from mme_display_fixtures import fixture
from mme_emm_paths_runner import EFFECT


CASES = ("incremental", "timer", "overlap")
GREEN_EFFECT = EFFECT.replace("float4(1, 0, 1, 1)", "float4(0, 1, 0, 1)")
MAX_EMM = 16 * 1024 * 1024


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
        return digest.hexdigest()


def fixture_files(root, case):
    folder = root / ("Projects" if case == "incremental" else "backups")
    target = "scene_002" if case == "incremental" else "timer_003" if case == "overlap" else "timer_002"
    projects = [root / "Projects/scene.pmm", folder / (target + ".pmm")]
    if case == "overlap":
        projects.append(folder / "timer_002.pmm")
    paths = [folder / ("scene_001.emm" if case == "incremental" else "timer_001.emm")]
    paths.extend(root / "Projects" / name for name in ("first.pmx", "second.pmx", "third.pmx", "green.fx"))
    for project in projects:
        paths.extend((project, project.with_suffix(".emm"), project.with_name(project.name + ".mms-plugins.json")))
    return paths


def evidence(root, case):
    return {path.relative_to(root).as_posix(): sha256(path) for path in fixture_files(root, case)
            if path.is_file()}


def normal_ref(value, anchor):
    # The runner constrains the fixture root to ASCII, so no ACP guessing is
    # needed for these references. Preserve and compare the complete paths.
    value = value.strip().strip('"')
    if not value or not value.isascii():
        return None
    return ntpath.normcase(ntpath.normpath(ntpath.join(anchor, value)))


def check_emm(path, model, effect):
    with path.open("rb") as stream:
        raw = stream.read(MAX_EMM + 1)
    if not raw or len(raw) > MAX_EMM:
        raise ValueError("invalid size for the generated EMM")
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        # Relevant fixture fields are ASCII even if unrelated MME text is not.
        text = raw.decode("utf-8-sig", errors="surrogateescape")
    if "\0" in text:
        raise ValueError("unexpected NUL in the generated EMM")
    section = ""
    objects = {}
    effects = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in ";#":
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        key, equal, value = line.partition("=")
        key = key.strip().lower()
        if not equal or not re.fullmatch(r"(?:pmd|acs)\d+", key):
            continue
        table = objects if section == "object" else effects if section == "effect" else None
        if table is not None:
            if key in table:
                raise ValueError(f"duplicate [{section}] {key} row")
            table[key] = value.strip()
    anchor = str(path.parent)
    expected_model = normal_ref(str(model), "")
    expected_fx = normal_ref(str(effect), "")
    if set(objects) != {"pmd1"} or normal_ref(objects["pmd1"], anchor) != expected_model:
        raise ValueError("EMM [Object] does not describe exactly the captured first.pmx")
    if set(effects) != {"pmd1"} or normal_ref(effects["pmd1"], anchor) != expected_fx:
        raise ValueError("EMM [Effect] does not bind exactly the captured green.fx to Pmd1")


def create_installation(root, sources, profile):
    root.mkdir()
    for name in ("Projects", "backups", "Data", "tmp", "local", "roaming"):
        (root / name).mkdir()
    for name in ("first.pmx", "second.pmx", "third.pmx"):
        (root / "Projects" / name).write_bytes(fixture())
    (root / "Projects/green.fx").write_text(GREEN_EFFECT, encoding="ascii")
    (root / "Data/mmconfig.ini").write_text("40\n40\n1000\n740\n0\n640\n480\n250\n1\n", encoding="ascii")
    (root / "mme-save-pair.fixture").write_text(profile + "\n", encoding="ascii")
    exe = root / "MMS-SaveProbe.exe"
    shutil.copy2(sources["host"], exe)
    shutil.copy2(sources["adapter"], root / "MMSAdapter.dll")
    package = root / "PLUGINS/effects/mme"
    package.mkdir(parents=True)
    shutil.copy2(sources["effect"], package / "MMEffect.dll")
    manifest = dict(schemaVersion=1, id="test.legacy.mme", name="Local MME", version="0.37.0",
                    kind="legacy_mme", entry="MMEffect.dll", architecture="x64",
                    minWindows="6.1.7601", reloadPolicy="restart")
    if "bridge" in sources:
        shutil.copy2(sources["bridge"], root / "MMHack.dll")
    else:
        shutil.copy2(sources["hack"], package / "MMHack.dll")
        manifest["hack"] = "MMHack.dll"
    (package / "plugin.json").write_text(json.dumps(manifest), encoding="ascii")
    (package / "MMEffect.ini").write_text("[System]\nSkipValidation=true\nEMMAutoSave=false\n", encoding="ascii")
    (root / "PLUGINS/config.json").write_text(
        json.dumps(dict(schemaVersion=1, maxDepth=2, disabledPlugins=[])), encoding="ascii")
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("MMS_", "MIKUMIKUSTUDIO_", "MIKUDANCESTUDIO_"))
           and key.upper() not in ("TEMP", "TMP", "LOCALAPPDATA", "APPDATA")}
    env.update(TEMP=str(root / "tmp"), TMP=str(root / "tmp"), LOCALAPPDATA=str(root / "local"),
               APPDATA=str(root / "roaming"), MMS_EFFECT_CACHE_DISABLE="1", MMS_NVIDIA_AA_FIX="0",
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(root / "autosave.ini"),
               MMS_RECENT_PROJECTS_FILE=str(root / "recents.txt"))
    return exe, env


def run_phase(root, exe, env, case, phase, profile):
    log = root / (phase + ".log")
    command = [str(exe), str(root), "1", "--mme-save-pair", phase, case]
    status = {"command": command, "log": str(log), "exitCode": None}
    with log.open("wb") as out:
        try:
            result = subprocess.run(command, cwd=root, env=env, stdin=subprocess.DEVNULL,
                                    stdout=out, stderr=subprocess.STDOUT, timeout=90,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            status["exitCode"] = result.returncode
        except (OSError, subprocess.TimeoutExpired) as exc:
            status["error"] = str(exc)
    text = log.read_text(encoding="utf-8", errors="replace")
    status["passed"] = (status["exitCode"] == 0 and
                        f"MME_SAVE_PAIR profile={profile}" in text.splitlines() and
                        f"PASS Windows real MME save pair case={case} phase={phase}" in text.splitlines())
    return status, text


def check_case(root, case, profile, before, after, save_log, reopen_log):
    if before != after:
        raise ValueError("the fresh reopen changed the PMM/EMM/sidecar, orphan, or input assets")
    if "MME_SAVE_PAIR write-phase-complete" not in save_log.splitlines():
        raise ValueError("save did not reach its source and prior-generation preservation assertions")
    if any(root.rglob("*.mms-save.json")):
        raise ValueError("a completed or definitely refused run left an unreconciled save journal")
    folder = root / ("Projects" if case == "incremental" else "backups")
    target = "scene_002" if case == "incremental" else "timer_003" if case == "overlap" else "timer_002"
    paths = [root / "Projects/scene.pmm"]
    if case == "overlap":
        paths.append(folder / "timer_002.pmm")
    target = folder / (target + ".pmm")
    lines = reopen_log.splitlines()
    decision = None
    if case == "overlap":
        decisions = [line for line in save_log.splitlines() if line.startswith("MME_SAVE_PAIR completion=")]
        if len(decisions) == 1:
            decision = re.fullmatch(r"MME_SAVE_PAIR completion=(saved|refused) pmm=([01]) emm=([01]) sidecar=([01])",
                                    decisions[0])
        if decision is None:
            raise ValueError("expected one reported overlap completion")
    outcomes = [line for line in lines if line.startswith("MME_SAVE_PAIR outcome=")]
    if outcomes == ["MME_SAVE_PAIR outcome=pair"]:
        if decision and decision.groups() != ("saved", "1", "1", "1" if profile == "full" else "0"):
            raise ValueError("overlap reported completion before both PMM and EMM and the expected sidecar state")
        paths.append(target)
    elif case == "overlap" and outcomes == ["MME_SAVE_PAIR outcome=absent"]:
        if decision.groups() != ("refused", "0", "0", "0"):
            raise ValueError("an absent overlap must be a definite refusal with no published members")
        if any(part.exists() for part in (target, target.with_suffix(".emm"),
                                          target.with_name(target.name + ".mms-plugins.json"))):
            raise ValueError("a refused overlap still published a member")
    else:
        raise ValueError("the fresh process did not confirm a captured pair or a definite safe refusal")
    for path in paths:
        if not path.is_file() or not path.with_suffix(".emm").is_file():
            raise ValueError(f"missing PMM/EMM member for {path.name}")
        sidecar = path.with_name(path.name + ".mms-plugins.json")
        if profile == "full":
            data = json.loads(sidecar.read_text(encoding="utf-8"))
            if not isinstance(data, dict) or data.get("mainSha256") != sha256(path):
                raise ValueError(f"plugin sidecar is not bound to the reopened PMM {path.name}")
        elif sidecar.exists():
            raise ValueError(f"unexpected plugin sidecar for {path.name}")
        check_emm(path.with_suffix(".emm"), root / "Projects/first.pmx", root / "Projects/green.fx")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True, help="scene_lifecycle_mme_tests Release executable")
    parser.add_argument("--adapter", type=Path, required=True, help="MMSAdapter.dll from the same build")
    parser.add_argument("--originals", type=Path, required=True, help="local original x64 MME 0.37 DLL directory")
    parser.add_argument("--bridge", type=Path, help="MMHack.dll from the same build; otherwise use original MMHack")
    parser.add_argument("--profile", choices=("full", "mmd932"), required=True)
    parser.add_argument("--output", type=Path, required=True, help="short ASCII evidence directory")
    parser.add_argument("--case", choices=CASES, action="append", help="run only this case; repeat as needed")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this test requires real x64 Windows, D3D9, and the original MME 0.37")
    sources = dict(host=args.host.resolve(), adapter=args.adapter.resolve(),
                   effect=args.originals.resolve() / "MMEffect.dll")
    if args.bridge:
        sources["bridge"] = args.bridge.resolve()
    else:
        sources["hack"] = args.originals.resolve() / "MMHack.dll"
    for name, path in sources.items():
        if not path.is_file():
            parser.error(f"missing {name}: {path}")
    output = args.output.resolve()
    if not str(output).isascii() or len(str(output)) > 120 or not re.fullmatch(r"[a-zA-Z]:\\.*", str(output)):
        parser.error("--output must be a short ASCII path on a local drive for the legacy PMM/EMM fields")
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="mme-save-", dir=output))
    hashes = {name: sha256(path) for name, path in sources.items()}
    report = {"profile": args.profile, "provider": "built-in-bridge" if args.bridge else "original-hack",
              "inputSha256": hashes, "cases": []}
    selected = tuple(dict.fromkeys(args.case)) if args.case else CASES
    for case in selected:
        root = run / case
        record = {"case": case, "passed": False}
        report["cases"].append(record)
        try:
            exe, env = create_installation(root, sources, args.profile)
            record["save"], save_log = run_phase(root, exe, env, case, "save", args.profile)
            before = record["beforeReopenSha256"] = evidence(root, case)
            if not record["save"]["passed"]:
                raise ValueError("save process failed; see save.log")
            record["reopen"], reopen_log = run_phase(root, exe, env, case, "reopen", args.profile)
            after = record["afterReopenSha256"] = evidence(root, case)
            if not record["reopen"]["passed"]:
                raise ValueError("fresh reopen process failed; see reopen.log")
            check_case(root, case, args.profile, before, after, save_log, reopen_log)
            record["passed"] = True
        except (OSError, ValueError) as exc:
            record["error"] = str(exc)
        print(json.dumps({"case": case, "passed": record["passed"], "error": record.get("error")}), flush=True)
    report["inputsUnchanged"] = all(path.is_file() and sha256(path) == hashes[name]
                                     for name, path in sources.items())
    report["passed"] = report["inputsUnchanged"] and all(case["passed"] for case in report["cases"])
    summary = run / "results.json"
    summary.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(summary)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
