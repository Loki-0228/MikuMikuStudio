"""Compare native Release builds with hidden D3D9 GPU-drained batches."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def positive_int(text):
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not an integer: {text!r}")
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be a positive integer: {text!r}")
    return value


def git(tree, *args):
    return subprocess.check_output(
        ["git", "-c", "safe.directory=" + tree.as_posix(), "-C", str(tree), *args],
        text=True, encoding="utf-8").strip()


def case_definitions(grass, character):
    cases = [(f"grass-{n}-spread", grass, ["--copies", str(n), "--layout", "spread"])
             for n in (1, 2, 4, 8)]
    cases += [("grass-4-overlap", grass, ["--copies", "4", "--layout", "overlap"]),
              ("face-static", character, ["--camera", "0,18.5,0,-5,0,0,0"]),
              ("face-moving", character, ["--camera", "0,18.5,0,-5,-6,0,0", "--orbit-degrees", "0.08"])]
    return cases


def select_cases(parser, all_cases, spec):
    if spec is None:
        return all_cases
    names = [name.strip() for name in spec.split(",")]
    if not names or any(not name for name in names):
        parser.error("--cases must be a non-empty comma-separated list of case names")
    known = {case for case, _, _ in all_cases}
    unknown = [name for name in names if name not in known]
    if unknown:
        parser.error("unknown case name(s): " + ", ".join(unknown)
                     + "; known cases: " + ", ".join(sorted(known)))
    wanted = set(names)
    return [case for case in all_cases if case[0] in wanted]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before-tree", "after-tree", "output", "grass", "character"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--cases", type=str, default=None,
                        help="comma-separated case names to run; default runs every case")
    parser.add_argument("--timeout", type=positive_int, default=180,
                        help="per-run subprocess timeout in seconds (default: 180)")
    args = parser.parse_args()
    if min(args.warmup, args.samples, args.cycles) < 1:
        parser.error("warmup, samples and cycles must be positive")
    cases = select_cases(parser, case_definitions(args.grass, args.character), args.cases)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = {k.upper(): v for k, v in os.environ.items()}
    env.pop("MIKUMIKUSTUDIO_TRACE_FILE", None)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    manifest = {
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "metric": "GPU-drained batch ms/frame; not playback FPS or GPU-only time",
        "protocol": "ABBA per case, sequential hidden windows, native D3D9, 1280x720",
        "warmup": args.warmup, "samples": args.samples, "cycles": args.cycles,
        "timeout_seconds": args.timeout,
        "cases_requested": [case for case, _, _ in cases],
        "script_sha256": sha(Path(__file__).resolve()),
        "environment": {k: v for k, v in env.items() if k.startswith(("OMP_", "MIKUMIKUSTUDIO_"))},
        "builds": {}, "assets": {}, "runs": [],
    }
    for model in (args.grass, args.character):
        manifest["assets"][str(model.resolve())] = {
            str(p.relative_to(model.parent)): sha(p)
            for p in sorted(model.parent.rglob("*")) if p.is_file()
        }
    for label, tree in (("before", args.before_tree), ("after", args.after_tree)):
        tree = tree.resolve()
        changes = git(tree, "status", "--porcelain", "--", "src", "include", "tests", "CMakeLists.txt")
        if changes:
            raise RuntimeError(f"uncommitted build inputs in {tree}: {changes}")
        source = tree / "build/tests/Release"
        for dll in ("d3d9.dll", "MMEffect.dll", "MMHack.dll"):
            if (source / dll).exists():
                raise RuntimeError(f"native benchmark directory contains {dll}")
        runtime = out / label
        runtime.mkdir()
        shutil.copy2(source / "frame_benchmark.exe", runtime)
        shutil.copy2(tree / "build/Release/MikuMikuDance.exe", runtime)
        # Both builds receive byte-identical assets, with hashes in the manifest.
        shutil.copytree(args.after_tree / "build/tests/Release/Data", runtime / "Data")
        manifest["builds"][label] = {
            "tree": str(tree), "commit": git(tree, "rev-parse", "HEAD"),
            "tree_status": git(tree, "status", "--porcelain"),
            "build_inputs_status": changes,
            "cmake_cache_sha256": sha(tree / "build/CMakeCache.txt"),
            "cmake_cache": (tree / "build/CMakeCache.txt").read_text(encoding="utf-8"),
            "runtime_files": {str(p.relative_to(runtime)): sha(p)
                              for p in sorted(runtime.rglob("*")) if p.is_file()},
        }
    manifest_path = out / "manifest.json"
    def save():
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    save()
    for case, model, extra in cases:
        for cycle in range(args.cycles):
            for index, label in enumerate(("before", "after", "after", "before")):
                stem = f"{case}-{cycle}-{index}-{label}"
                runtime = out / label
                command = [str(runtime / "frame_benchmark.exe"), str(model.resolve()),
                           "--throughput", "--viewport", "1280x720", "--warmup", str(args.warmup),
                           "--samples", str(args.samples), "--json", str(out / (stem + ".json")), *extra]
                run = {"case": case, "cycle": cycle, "label": label, "command": command,
                       "log": stem + ".log", "json": stem + ".json"}
                manifest["runs"].append(run)
                save()
                with (out / run["log"]).open("w", encoding="utf-8") as log:
                    try:
                        result = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                                stderr=subprocess.STDOUT, timeout=args.timeout,
                                                startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)
                        run["exit_code"] = result.returncode
                    except subprocess.TimeoutExpired:
                        run["error"] = "timeout; subprocess stopped"
                        save()
                        raise
                save()
                if result.returncode:
                    raise RuntimeError(f"{stem} failed ({result.returncode}); original output retained")
                data = json.loads((out / run["json"]).read_text(encoding="utf-8"))
                if not data["model_pass_viewport"]["ok"] or data["timing"]["sample_count"] != args.samples:
                    raise RuntimeError(f"invalid viewport or incomplete samples: {stem}")
                run["ms"] = data["timing"]["drained_batch_ms_per_frame"]
                save()
                print(stem, f'{run["ms"]:.4f} ms', flush=True)
    summary = []
    for case, _, _ in cases:
        row = {"case": case}
        for label in ("before", "after"):
            values = [r["ms"] for r in manifest["runs"] if r["case"] == case and r["label"] == label]
            row[label] = {"median_ms": statistics.median(values), "min_ms": min(values),
                          "max_ms": max(values), "values": values}
        row["change_percent"] = 100 * (row["after"]["median_ms"] / row["before"]["median_ms"] - 1)
        summary.append(row)
    (out / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
