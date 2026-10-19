"""Run matched D3D9 frame benchmarks in ABBA order and retain raw results."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--grass", type=Path, required=True)
    parser.add_argument("--grass-default", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--copies", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--layouts", nargs="+", choices=["spread", "overlap"],
                        default=["spread", "overlap"])
    parser.add_argument("--modes", nargs="+", choices=["native", "mme-default", "mme-effect"],
                        default=["native", "mme-default", "mme-effect"])
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--samples", type=int, default=300)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if any(n not in (1, 2, 4, 8) for n in args.copies) or min(args.rounds, args.warmup, args.samples) < 1:
        parser.error("copies must be 1/2/4/8; rounds and frame counts must be positive")
    args.out.mkdir(parents=True, exist_ok=True)
    if (args.out / "manifest.json").exists():
        parser.error("use a new output directory to preserve previous results")

    environment = {k: v for k, v in os.environ.items()
                   if k.startswith(("MIKUMIKUSTUDIO_", "OMP_"))}
    if environment.get("MIKUMIKUSTUDIO_SKIP_PHYSICS", "0") not in ("", "0"):
        parser.error("physics must remain enabled")
    builds = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    manifest = {"created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                "environment": environment, "builds": {}, "runs": []}
    for name, root in builds.items():
        revision = subprocess.check_output(
            ["git", "-c", f"safe.directory={root.as_posix()}", "-C", str(root), "rev-parse", "HEAD"],
            text=True).strip()
        binaries = {
            "native": root / "build/tests/Release/frame_benchmark.exe",
            "mme": root / "build/tests/mme-benchmark/Release/MikuMikuDance.exe",
        }
        cache = (root / "build/CMakeCache.txt").read_text(encoding="utf-8")
        for switch in ("MIKUMIKUSTUDIO_PERF:BOOL=OFF", "MIKUMIKUSTUDIO_DIAG:BOOL=OFF",
                       "CMAKE_CXX_FLAGS_RELEASE:STRING=/O2 /Ob2 /DNDEBUG"):
            if switch not in cache:
                raise RuntimeError(f"{root}: required configuration missing: {switch}")
        manifest["builds"][name] = {"root": str(root), "commit": revision,
            "revision_note": "commit is the worktree HEAD at run start, not a binary build stamp; retain the build log and executable hash",
            "worktree_status": subprocess.check_output(
                ["git", "-c", f"safe.directory={root.as_posix()}", "-C", str(root),
                 "status", "--short"], text=True), "binaries": {
            kind: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                   "modified_unix_seconds": path.stat().st_mtime}
            for kind, path in binaries.items()}}

    def save() -> None:
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    save()
    summaries = []
    for mode in args.modes:
        model = (args.grass_default if mode == "mme-default" else args.grass).resolve()
        for layout in args.layouts:
            for copies in args.copies:
                measurements = {"A": [], "B": []}
                signatures = []
                for repeat in range(args.rounds):
                    for position, variant in enumerate("ABBA"):
                        label = f"{mode}-{layout}-{copies}-r{repeat + 1}-{position + 1}{variant}"
                        binary_kind = "native" if mode == "native" else "mme"
                        exe = Path(manifest["builds"][variant]["binaries"][binary_kind]["path"])
                        result_file = (args.out / f"{label}.json").resolve()
                        command = [str(exe), str(model), "--throughput", "--copies", str(copies),
                                   "--layout", layout, "--viewport", "1280x720",
                                   "--warmup", str(args.warmup), "--samples", str(args.samples),
                                   "--json", str(result_file)]
                        print(f"START {label}", flush=True)
                        started = time.monotonic()
                        with (args.out / f"{label}.log").open("wb") as log:
                            try:
                                run = subprocess.run(command, cwd=exe.parent, stdout=log,
                                                     stderr=subprocess.STDOUT, timeout=args.timeout)
                            except subprocess.TimeoutExpired:
                                manifest["runs"].append({"label": label, "variant": variant,
                                    "command": command, "exit_code": "timeout",
                                    "wall_seconds": time.monotonic() - started})
                                save()
                                raise
                        record = {"label": label, "variant": variant, "command": command,
                                  "exit_code": run.returncode, "wall_seconds": time.monotonic() - started}
                        manifest["runs"].append(record)
                        save()
                        if run.returncode:
                            raise RuntimeError(f"{label} exited {run.returncode}; see saved log")
                        result = json.loads(result_file.read_text(encoding="utf-8-sig"))
                        if result["scene"]["models"] != copies or result["perf_timing_compiled"]:
                            raise RuntimeError(f"{label}: unexpected model count or timing instrumentation")
                        if (result["viewport"]["width"], result["viewport"]["height"]) != (1280, 720):
                            raise RuntimeError(f"{label}: mismatched viewport")
                        if not all(result.get(k) for k in
                                   ("all_centers_in_view", "samples_complete", "framed", "placement_verified")):
                            raise RuntimeError(f"{label}: model placement verification failed")
                        if not result.get("model_pass_viewport", {}).get("ok"):
                            raise RuntimeError(f"{label}: viewport verification failed")
                        placements = [{k: p[k] for k in ("slot", "offset", "center") if k in p}
                                      for p in result.get("placements") or []]
                        signatures.append({"gpu": result["gpu"], "camera": result["camera"],
                                           "placements": placements, "scene": result["scene"]})
                        if signatures[-1] != signatures[0]:
                            raise RuntimeError(f"{label}: scene, camera, layout or GPU changed between builds")
                        measurements[variant].append(result["timing"]["drained_batch_ms_per_frame"])
                        print(f"DONE {label}: {measurements[variant][-1]:.3f} ms/frame", flush=True)
                a, b = (statistics.median(measurements[key]) for key in ("A", "B"))
                spans = {key: (max(values) - min(values)) / statistics.median(values) * 100
                         for key, values in measurements.items()}
                paired = []
                for repeat in range(args.rounds):
                    av = statistics.mean(measurements["A"][repeat * 2:repeat * 2 + 2])
                    bv = statistics.mean(measurements["B"][repeat * 2:repeat * 2 + 2])
                    paired.append((bv / av - 1) * 100)
                unstable = max(spans.values()) > 5
                summaries.append({"mode": mode, "layout": layout, "copies": copies,
                                  "baseline_ms": a, "candidate_ms": b,
                                  "change_percent": (b / a - 1) * 100,
                                  "baseline_drained_fps": 1000 / a, "candidate_drained_fps": 1000 / b,
                                  "raw_ms": measurements,
                                  "within_variant_span_percent": spans,
                                  "abba_round_change_percent": paired,
                                  "unstable": unstable,
                                  "note": "GPU-drained batch throughput; not window FPS. Positive change is slower. "
                                          + ("Load variation exceeds 5%; repeat before making a performance claim."
                                             if unstable else "Small changes still require repeatable evidence above noise.")})
                (args.out / "summary.json").write_text(json.dumps(summaries, indent=2), encoding="utf-8")
                print(json.dumps(summaries[-1]), flush=True)


if __name__ == "__main__":
    main()
