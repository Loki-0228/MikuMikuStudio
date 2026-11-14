#!/usr/bin/env python3
"""Offline acceptance and cache-evidence harness for the pinned full builds."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import statistics
import struct
import sys
import tempfile

MAIN = "2381bafc40ed864cf15f39278f72bada600d96a7"
ABI = "bc2b7a795d17a942e5c1ed95c3878f8ba0e5c28f"
VARIANTS = ("main", "abi")
PHASES = ("app-cache-cold", "disk-repeat-new-process", "repeat-same-process")
STEPS = tuple((phase, point) for phase in PHASES for point in ("before", "after"))
METRICS = ("pmm_open", "model_prepare", "texture_prepare", "effect_accept", "total_ready")
ROOT_PREFIX = "mms-load-v1-"
HEX = re.compile(r"[0-9a-f]{64}\Z")
SESSION = re.compile(r"[0-9a-f]{32}\Z")
SEGMENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}\Z")
RESERVED = {"aux", "nul", "con", "prn", *(f"{p}{n}" for p in ("com", "lpt") for n in range(10))}
MAX_METADATA = 32 * 1024 * 1024
MIB = 1024 * 1024


class Invalid(ValueError):
    pass


def require(condition, what):
    if not condition:
        raise Invalid(what)


def fields(value, names, label):
    require(isinstance(value, dict) and value.keys() == set(names), f"{label}: unexpected or missing fields")


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def sha(value):
    return isinstance(value, str) and HEX.fullmatch(value) is not None


def canonical(obj):
    return json.dumps(obj, ensure_ascii=True, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(obj):
    return hashlib.sha256(canonical(obj)).hexdigest()


def relative(value):
    require(isinstance(value, str) and len(value) <= 200, "unsafe or overlong relative path")
    require(all(SEGMENT.fullmatch(part) and not part.endswith(".") and
                part.split(".")[0].lower() not in RESERVED for part in value.split("/")),
            "unsafe relative path (use portable, sanitized ASCII names)")
    return value


def safe_stat(path):
    info = path.lstat()
    require(not stat.S_ISLNK(info.st_mode) and not
            (getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT),
            "symlink or reparse point is not supported")
    return info


def directory(path):
    require(stat.S_ISDIR(safe_stat(path).st_mode), "expected a regular directory")
    return path


def checked_path(path):
    # Check every component before resolving it, including existing sandbox parents.
    path = Path(os.path.abspath(path))
    for item in (*reversed(path.parents), path):
        safe_stat(item)
    return path


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def read_stable(path, prefix=None, copy_to=None):
    before = safe_stat(path)
    require(stat.S_ISREG(before.st_mode), "expected a regular file")
    h = hashlib.sha256()
    leading = b""
    with path.open("rb") as stream:
        require(identity(os.fstat(stream.fileno())) == identity(before), "input changed during open")
        if prefix:
            leading = stream.read(prefix)
            stream.seek(0)
        target = copy_to.open("xb") if copy_to else None
        try:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(block)
                if target:
                    target.write(block)
        finally:
            if target:
                target.close()
        require(identity(os.fstat(stream.fileno())) == identity(before), "input changed during read")
    require(identity(safe_stat(path)) == identity(before), "input changed during read")
    return {"bytes": before.st_size, "sha256": h.hexdigest()}, leading


def inventory(root, *, copy_to=None, strict_names=False, observe=None):
    """Hash sorted relative filenames and contents. Reject ambiguous filesystem objects."""
    entries = {}
    seen = set()

    def scan(path, parent=""):
        directory(path)
        for child in sorted(path.iterdir(), key=lambda obj: obj.name):
            rel = f"{parent}/{child.name}" if parent else child.name
            if strict_names:
                relative(rel)
            key = rel.casefold()
            require(key not in seen, "case-insensitive duplicate filesystem entry")
            seen.add(key)
            info = safe_stat(child)
            dest = copy_to / rel if copy_to is not None else None
            if stat.S_ISDIR(info.st_mode):
                if dest is not None:
                    dest.mkdir()
                scan(child, rel)
            else:
                require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                        "fixture and cache inputs must be regular, non-hardlinked files")
                fingerprint, head = read_stable(child, prefix=80 if observe else None, copy_to=dest)
                entries[rel] = fingerprint
                if observe:
                    observe(rel, fingerprint["bytes"], head)
                if dest is not None:
                    dest.chmod(0o444)

    scan(root)
    return entries


def read_json(path):
    path = checked_path(path)
    before = safe_stat(path)
    require(stat.S_ISREG(before.st_mode) and before.st_size <= MAX_METADATA, "invalid JSON input file size")

    def reject_duplicates(pairs):
        record = {}
        for key, value in pairs:
            require(key not in record, "duplicate key in JSON")
            record[key] = value
        return record

    def reject_non_finite(_):
        raise Invalid("non-finite JSON number")

    with path.open("rb") as stream:
        require(identity(os.fstat(stream.fileno())) == identity(before), "JSON input changed during open")
        raw = stream.read(MAX_METADATA + 1)
    require(len(raw) <= MAX_METADATA and identity(safe_stat(path)) == identity(before),
            "JSON input changed during read")
    return json.loads(raw.decode("utf-8"), object_pairs_hook=reject_duplicates, parse_constant=reject_non_finite)


def write_json(path, obj):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(obj, stream, sort_keys=True, ensure_ascii=True, indent=2, allow_nan=False)
        stream.write("\n")


def executable(path, expected, product):
    require(sha(expected), "executable SHA-256 must be 64 lowercase hexadecimal digits")
    path = checked_path(path)
    fingerprint, header = read_stable(path, prefix=65560 if product else None)
    require(fingerprint["sha256"] == expected, "executable SHA-256 mismatch")
    if product:
        require(path.suffix.lower() == ".exe" and len(header) >= 64 and header[:2] == b"MZ",
                "application runs require independently pinned Windows .exe files")
        offset = struct.unpack_from("<I", header, 0x3c)[0]
        require(64 <= offset and offset + 26 <= len(header) and
                header[offset:offset + 6] == b"PE\0\0\x64\x86" and
                header[offset + 24:offset + 26] == b"\x0b\x02",
                "expected Windows x86-64 PE32+")
    return fingerprint


def validate_outcome(outcome, fixture):
    """Contract for sanitized observations; model order is deliberately significant."""
    fields(outcome, ("pmm", "models", "effects"), "outcome")
    pmm = outcome["pmm"]
    fields(pmm, ("fixture", "version", "result", "active_model_count"), "PMM outcome")
    name = relative(pmm["fixture"])
    require(name in fixture and name.lower().endswith(".pmm"), "PMM is not part of pinned fixture")
    require(isinstance(pmm["version"], str) and re.fullmatch(r"[0-9]{4}", pmm["version"]), "invalid PMM version")
    require(pmm["result"] in ("opened", "rejected"), "invalid PMM outcome")
    count = pmm["active_model_count"]
    require(integer(count, 0, 1_000_000), "invalid PMM active model count")
    models = outcome["models"]
    require(isinstance(models, list) and len(models) == count, "model count does not match PMM state")
    for item in models:
        fields(item, ("fixture", "vertices", "indices", "materials", "semantic_sha256"), "model outcome")
        name = relative(item["fixture"])
        require(name in fixture and name.lower().endswith((".pmd", ".pmx")), "model is not part of pinned fixture")
        require(all(integer(item[k], 0, 2**32 - 1) for k in ("vertices", "indices", "materials")),
                "invalid model output count")
        require(sha(item["semantic_sha256"]), "model semantic digest missing or invalid")
    effects = outcome["effects"]
    require(isinstance(effects, dict), "invalid effect map")
    for name, status in effects.items():
        require(relative(name) in fixture and name.lower().endswith((".fx", ".fxsub", ".fxc")),
                "effect is not part of pinned fixture")
        require(status in ("accepted", "rejected"), "invalid effect acceptance outcome")


def validate_fixture(root, manifest):
    observed = inventory(root / "fixture", strict_names=True)
    require(observed == manifest["fixture"], "read-only fixture bytes/path set changed")


def validate_pmm_header(fixture, expected):
    project = fixture / expected["pmm"]["fixture"]
    _, header = read_stable(project, prefix=30)
    require(len(header) == 30 and header[:19] == b"Polygon Movie maker" and
            header[20:25] == expected["pmm"]["version"].encode("ascii") + b"\0",
            "application fixture PMM header does not match the declared version")


def prepared_root(path):
    return directory(checked_path(path))


def load_run(path):
    root = prepared_root(path)
    require(root.name.startswith(ROOT_PREFIX), "not a harness-owned run directory")
    manifest = read_json(root / "plan.json")
    fields(manifest, ("schema", "run_id", "mode", "profile", "scope", "pairs", "builds",
                      "fixture", "fixture_sha256", "expected_outcome"), "plan")
    require(type(manifest["schema"]) is int and manifest["schema"] == 1 and manifest["run_id"] == root.name,
            "plan does not match run directory")
    require(manifest["mode"] in ("application", "synthetic") and manifest["profile"] == "full" and
            manifest["scope"] == "ordinary-pmm-open" and integer(manifest["pairs"], 1, 30), "invalid plan")
    fields(manifest["builds"], VARIANTS, "build pins")
    for variant, commit in (("main", MAIN), ("abi", ABI)):
        build = manifest["builds"][variant]
        fields(build, ("commit", "source_sha256", "exe"), "build")
        require(build["commit"] == commit and
                (build["source_sha256"] is None if variant == "main" else sha(build["source_sha256"])),
                "source provenance does not match requested baselines")
        fields(build["exe"], ("bytes", "sha256"), "executable")
        require(integer(build["exe"]["bytes"], 1, 2**63 - 1) and sha(build["exe"]["sha256"]), "invalid executable pin")
    require(manifest["builds"]["main"]["exe"]["sha256"] != manifest["builds"]["abi"]["exe"]["sha256"],
            "comparison requires two distinct binaries")
    require(isinstance(manifest["fixture"], dict) and
            digest(manifest["fixture"]) == manifest["fixture_sha256"], "fixture inventory digest mismatch")
    for name, item in manifest["fixture"].items():
        relative(name)
        fields(item, ("bytes", "sha256"), "fixture file")
        require(integer(item["bytes"], 0, 2**63 - 1) and sha(item["sha256"]), "invalid fixture file digest")
    validate_outcome(manifest["expected_outcome"], manifest["fixture"])
    return root, manifest


def sample_path(root, manifest, variant, pair):
    require(variant in VARIANTS and integer(pair, 1, manifest["pairs"]), "unknown variant or pair")
    return directory(checked_path(root / variant / f"pair-{pair:02d}"))


def prepare(args):
    parent = prepared_root(args.output_parent)
    fixture = prepared_root(args.fixture_dir)
    require(parent != fixture and fixture not in parent.parents, "run output cannot be inside the fixture")
    require(integer(args.pairs, 1, 30) and sha(args.abi_source_sha256), "invalid pair count or ABI source SHA-256")
    pins = {
        "main": {"commit": MAIN, "source_sha256": None,
                 "exe": executable(args.main_exe, args.main_sha256, args.mode == "application")},
        "abi": {"commit": ABI, "source_sha256": args.abi_source_sha256,
                "exe": executable(args.abi_exe, args.abi_sha256, args.mode == "application")},
    }
    require(pins["main"]["exe"]["sha256"] != pins["abi"]["exe"]["sha256"], "comparison requires two distinct binaries")
    expected = read_json(args.expectations)
    initial = inventory(fixture, strict_names=True)
    validate_outcome(expected, initial)
    if args.mode == "application":
        validate_pmm_header(fixture, expected)
    root = Path(tempfile.mkdtemp(prefix=ROOT_PREFIX, dir=parent))
    shared = root / "fixture"
    shared.mkdir()
    require(inventory(fixture, copy_to=shared, strict_names=True) == initial, "source fixture changed while staging")
    for variant in VARIANTS:
        for pair in range(1, args.pairs + 1):
            sample = root / variant / f"pair-{pair:02d}"
            for part in ("cache/MME", "cache/Textures", "settings", "logs", "checkpoints", "runtime"):
                (sample / part).mkdir(parents=True, exist_ok=True)
    write_json(root / "plan.json", {
        "schema": 1, "run_id": root.name, "mode": args.mode, "profile": "full",
        "scope": "ordinary-pmm-open", "pairs": args.pairs, "builds": pins,
        "fixture": initial, "fixture_sha256": digest(initial), "expected_outcome": expected,
    })
    return root


def adapter_paths(root, manifest, variant, pair):
    sample = sample_path(root, manifest, variant, pair)
    cache = directory(checked_path(sample / "cache"))
    settings = directory(checked_path(sample / "settings"))
    logs = directory(checked_path(sample / "logs"))
    return {
        "fixture_dir": str(directory(checked_path(root / "fixture"))),
        "runtime_dir": str(directory(checked_path(sample / "runtime"))),
        "env": {
            "MMS_CACHE_MANAGER_DIR": str(cache),
            "MMS_EFFECT_CACHE_DIR": str(directory(checked_path(cache / "MME"))),
            "MMS_TEXTURE_CACHE_DIR": str(directory(checked_path(cache / "Textures"))),
            "MMS_CACHE_MANAGER_SETTINGS": str(settings / "cache-manager.ini"),
            "MMS_RECENT_PROJECTS_FILE": str(settings / "recent-projects.txt"),
            "MIKUMIKUSTUDIO_TRACE_FILE": str(logs / "trace.log"),
        },
    }


def cache_snapshot(sample):
    root = directory(checked_path(sample / "cache"))
    payload = {"effect_fxb": 0, "texture_mtc": 0}

    def observe(name, size, head):
        # Exclude empty files, arbitrary extensions and wrong-format headers.
        # This is sufficient for persisted-candidate presence, not proof of a hit.
        parts = name.lower().split("/")
        if len(parts) != 3:
            return
        stem, extension = os.path.splitext(parts[-1])
        if not sha(stem):
            return
        if parts[0] == "mme" and extension == ".fxb" and len(head) >= 80:
            magic, version, key_bytes, blob_bytes = struct.unpack_from("<4sIII", head)
            if (magic == b"MMEC" and version == 3 and 0 < key_bytes <= 16 * MIB and
                    0 < blob_bytes <= 16 * MIB and size == 80 + key_bytes + blob_bytes and head[16:48].hex() == stem):
                payload["effect_fxb"] += 1
        if parts[0] == "textures" and extension == ".mtc" and len(head) >= 76:
            magic, version, blob_bytes = struct.unpack_from("<4sII", head)
            if (magic == b"MPTC" and version == 2 and 48 <= blob_bytes <= 128 * MIB + 48 and
                    size == 76 + blob_bytes and head[12:44].hex() == stem):
                payload["texture_mtc"] += 1

    entries = inventory(root, observe=observe)
    return {"sha256": digest(entries), "file_count": len(entries), "payload_files": payload}


def checkpoint_path(sample, phase, point):
    require((phase, point) in STEPS, "unknown checkpoint")
    return directory(checked_path(sample / "checkpoints")) / f"{phase}.{point}.json"


def read_checkpoint(sample, manifest, variant, pair, phase, point):
    record = read_json(checkpoint_path(sample, phase, point))
    fields(record, ("run_id", "variant", "pair", "phase", "point", "fixture_sha256", "cache"), "checkpoint")
    require((record["run_id"], record["variant"], record["pair"], record["phase"], record["point"],
             record["fixture_sha256"]) == (manifest["run_id"], variant, pair, phase, point, manifest["fixture_sha256"]),
            "checkpoint identity mismatch")
    cache = record["cache"]
    fields(cache, ("sha256", "file_count", "payload_files"), "checkpoint cache")
    require(sha(cache["sha256"]) and integer(cache["file_count"], 0, 1_000_000), "invalid cache snapshot")
    counts = cache["payload_files"]
    fields(counts, ("effect_fxb", "texture_mtc"), "cache payload counts")
    require(all(integer(n, 0, cache["file_count"]) for n in counts.values()) and
            sum(counts.values()) <= cache["file_count"], "invalid payload counts")
    return cache


def verify_before(phase, cache, previous):
    if phase == PHASES[0]:
        require(cache == {"sha256": digest({}), "file_count": 0,
                          "payload_files": {"effect_fxb": 0, "texture_mtc": 0}},
                "cold checkpoint already contains cache files; use a new run")
    else:
        require(cache == previous, "cache snapshot changed between operations (not a controlled repeat)")


def checkpoint(root, manifest, variant, pair, phase, point):
    sample = sample_path(root, manifest, variant, pair)
    require((phase, point) in STEPS, "unknown checkpoint")
    idx = STEPS.index((phase, point))
    previous = read_checkpoint(sample, manifest, variant, pair, *STEPS[idx - 1]) if idx else None
    validate_fixture(root, manifest)
    snap = cache_snapshot(sample)
    if point == "before":
        verify_before(phase, snap, previous)
    path = checkpoint_path(sample, phase, point)
    write_json(path, {"run_id": manifest["run_id"], "variant": variant, "pair": pair,
                      "phase": phase, "point": point, "fixture_sha256": manifest["fixture_sha256"], "cache": snap})
    return path


def first_differences(expected, actual, path="outcome"):
    if isinstance(expected, list) and isinstance(actual, list):
        return ([(path + ".length", len(expected), len(actual))] if len(expected) != len(actual) else []) + [
            difference for index, (a, b) in enumerate(zip(expected, actual))
            for difference in first_differences(a, b, f"{path}[{index}]")]
    if isinstance(expected, dict) and isinstance(actual, dict):
        return [
            difference for key in sorted(expected.keys() | actual.keys())
            for difference in first_differences(expected.get(key), actual.get(key), f"{path}.{key}")]
    return [(path, expected, actual)] if expected != actual else []


def read_observations(path, manifest):
    document = read_json(path)
    fields(document, ("schema", "run_id", "mode", "fixture_sha256", "capture", "records"), "observations")
    require(type(document["schema"]) is int and document["schema"] == 1 and
            document["run_id"] == manifest["run_id"] and document["mode"] == manifest["mode"] and
            document["fixture_sha256"] == manifest["fixture_sha256"], "observation header does not identify this plan")
    capture = document["capture"]
    fields(capture, ("host_label", "adapter_sha256", "timing_clock"), "capture")
    require(isinstance(capture["host_label"], str) and SEGMENT.fullmatch(capture["host_label"]) and
            sha(capture["adapter_sha256"]) and capture["timing_clock"] in ("monotonic_in_process", "unavailable"),
            "invalid capture identity or timing clock")
    rows = document["records"]
    require(isinstance(rows, list) and len(rows) == 6 * manifest["pairs"], "wrong number of observations")
    index = {}
    for row in rows:
        fields(row, ("variant", "pair", "phase", "exe_sha256", "process_session", "elapsed_ns", "outcome"), "observation")
        variant, pair, phase = row["variant"], row["pair"], row["phase"]
        require(variant in VARIANTS and integer(pair, 1, manifest["pairs"]) and phase in PHASES, "invalid observation slot")
        label = f"{variant}/pair-{pair:02d}/{phase}"
        require(row["exe_sha256"] == manifest["builds"][variant]["exe"]["sha256"], f"{label}: wrong executable")
        session = row["process_session"]
        require(isinstance(session, str) and SESSION.fullmatch(session), f"{label}: invalid process session")
        timing = row["elapsed_ns"]
        fields(timing, METRICS, f"{label}: timings")
        require(all(value is None or integer(value, 1, 2**63 - 1) for value in timing.values()),
                f"{label}: invalid duration; use null when not measured")
        require(capture["timing_clock"] != "unavailable" or all(value is None for value in timing.values()),
                f"{label}: timings supplied without a supported capture clock")
        require(timing["total_ready"] is None or all(value is None or value <= timing["total_ready"]
                for metric, value in timing.items() if metric != "total_ready"),
                f"{label}: stage duration exceeds total")
        validate_outcome(row["outcome"], manifest["fixture"])
        key = (variant, pair, phase)
        require(key not in index, f"{label}: duplicate observation")
        index[key] = row
    return index, capture


def middle(values):
    return statistics.median(values) if values else None


def compare(root, manifest, args):
    validate_fixture(root, manifest)
    for variant in VARIANTS:
        expected = manifest["builds"][variant]["exe"]
        actual = executable(getattr(args, f"{variant}_exe"), expected["sha256"], manifest["mode"] == "application")
        require(expected == actual, "binary differs from fingerprint")
    observations, capture = read_observations(args.observations, manifest)
    differences = []
    rows = []
    sessions = set()
    for variant in VARIANTS:
        for pair in range(1, manifest["pairs"] + 1):
            sample = sample_path(root, manifest, variant, pair)
            cold, warm, repeat = (observations[(variant, pair, phase)] for phase in PHASES)
            first, second, third = (row["process_session"] for row in (cold, warm, repeat))
            require(first != second and first not in sessions and second not in sessions and second == third,
                    "process sessions do not show cold/new-process/same-process independence")
            sessions.update((first, second))
            previous = None
            for phase in PHASES:
                before = read_checkpoint(sample, manifest, variant, pair, phase, "before")
                after = read_checkpoint(sample, manifest, variant, pair, phase, "after")
                verify_before(phase, before, previous)
                previous = after
                row = observations[(variant, pair, phase)]
                outcome_diff = first_differences(manifest["expected_outcome"], row["outcome"])
                for field, expected, actual in outcome_diff:
                    differences.append({"variant": variant, "pair": pair, "phase": phase,
                                        "field": field, "expected": expected, "actual": actual})
                rows.append({**row, "cache_before": before, "cache_after": after,
                             "outcome_matches_fixture": not outcome_diff,
                             "disk_payload_before": any(before["payload_files"].values())})
            require(cache_snapshot(sample) == previous, "cache changed after final checkpoint")

    indexed = {(r["variant"], r["pair"], r["phase"]): r for r in rows}
    metrics = {}
    for phase in PHASES:
        metrics[phase] = {}
        for metric in METRICS:
            pairs = []
            disqualified = {"missing_telemetry": 0, "outcome_mismatch": 0, "disk_payload_absent": 0}
            for number in range(1, manifest["pairs"] + 1):
                a, b = [indexed[(v, number, phase)] for v in VARIANTS]
                if not a["outcome_matches_fixture"] or not b["outcome_matches_fixture"]:
                    disqualified["outcome_mismatch"] += 1
                elif phase == PHASES[1] and not (a["disk_payload_before"] and b["disk_payload_before"]):
                    disqualified["disk_payload_absent"] += 1
                elif phase == PHASES[1] and (
                    metric == "effect_accept" and not all(r["cache_before"]["payload_files"]["effect_fxb"] for r in (a, b))
                    or metric == "texture_prepare" and not all(r["cache_before"]["payload_files"]["texture_mtc"] for r in (a, b))
                ):
                    disqualified["disk_payload_absent"] += 1
                elif a["elapsed_ns"][metric] is None or b["elapsed_ns"][metric] is None:
                    disqualified["missing_telemetry"] += 1
                else:
                    pairs.append((a["elapsed_ns"][metric], b["elapsed_ns"][metric]))
            metrics[phase][metric] = {
                "pairs_compared": len(pairs), "pairs_excluded": disqualified,
                "main_median_ns": middle([a for a, b in pairs]),
                "abi_median_ns": middle([b for a, b in pairs]),
                "median_paired_abi_minus_main_ns": middle([b - a for a, b in pairs]),
            }

    report = {"schema": 1, "run_id": manifest["run_id"], "mode": manifest["mode"], "profile": manifest["profile"],
              "scope": manifest["scope"], "builds": manifest["builds"], "fixture_sha256": manifest["fixture_sha256"],
              "fixture": manifest["fixture"], "expected_outcome": manifest["expected_outcome"],
              "capture": capture, "differences": differences, "observations": rows, "timing": metrics,
              "performance_verdict": "not_evaluated",
              "evidence_origin": "external_application_adapter" if manifest["mode"] == "application" else "synthetic",
              "cache_hit_verdict": "not_evaluated",
              "semantic_verdict": "mismatch" if differences else "observations_match_fixture"}
    report_dir = Path(tempfile.mkdtemp(prefix="report-", dir=root))
    write_json(report_dir / "report.json", report)
    cache_fields = ("effect_fxb", "texture_mtc")
    columns = ("mode", "variant", "pair", "phase", "exe_sha256", "fixture_sha256", "process_session",
               "outcome_matches_fixture", "disk_payload_before", *(f"{m}_ns" for m in METRICS),
               *(f"{point}_{key}" for point in ("before", "after") for key in cache_fields))
    with (report_dir / "observations.csv").open("x", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            line = {k: row[k] for k in columns if k in row}
            line.update(mode=manifest["mode"], fixture_sha256=manifest["fixture_sha256"])
            line.update({f"{metric}_ns": value for metric, value in row["elapsed_ns"].items()})
            line.update({f"{point}_{key}": row[f"cache_{point}"]["payload_files"][key]
                         for point in ("before", "after") for key in cache_fields})
            writer.writerow(line)
    return report_dir, 1 if differences else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="command", required=True)
    plan = subs.add_parser("prepare", help="validate independent pins and stage fixture/cache roots")
    for arg in ("output-parent", "fixture-dir", "expectations", "main-exe", "main-sha256", "abi-exe",
                "abi-sha256", "abi-source-sha256"):
        plan.add_argument(f"--{arg}", required=True)
    plan.add_argument("--pairs", type=int, default=5)
    plan.add_argument("--mode", choices=("application", "synthetic"), default="application")
    for command in ("paths", "checkpoint", "compare"):
        sub = subs.add_parser(command)
        sub.add_argument("run_dir")
        if command != "compare":
            sub.add_argument("variant", choices=VARIANTS)
            sub.add_argument("pair", type=int)
        if command == "checkpoint":
            sub.add_argument("phase", choices=PHASES)
            sub.add_argument("point", choices=("before", "after"))
        if command == "compare":
            for arg in ("main-exe", "abi-exe", "observations"):
                sub.add_argument(f"--{arg}", required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "prepare":
            print(prepare(args))
        else:
            root, manifest = load_run(args.run_dir)
            if args.command == "paths":
                print(json.dumps(adapter_paths(root, manifest, args.variant, args.pair), indent=2))
            elif args.command == "checkpoint":
                print(checkpoint(root, manifest, args.variant, args.pair, args.phase, args.point))
            else:
                report_dir, code = compare(root, manifest, args)
                print(report_dir)
                return code
    except (Invalid, OSError, ValueError, RecursionError) as exc:
        print(f"invalid or incomplete evidence: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
