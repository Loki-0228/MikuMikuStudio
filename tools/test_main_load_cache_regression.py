#!/usr/bin/env python3
"""Run with python -B tools/test_main_load_cache_regression.py."""
import csv
import hashlib
from contextlib import redirect_stdout, redirect_stderr
from io import StringIO
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

import main_load_cache_regression as load


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def write_synthetic_cache(path, texture):
    key_data = b"synthetic cache key"
    key_hash = hashlib.sha256(key_data).digest()
    payload = bytes(48) + b"synthetic cache payload"
    payload_hash = hashlib.sha256(payload).digest()
    if texture:
        header = struct.pack("<4sII", b"MPTC", 2, len(payload))
    else:
        header = struct.pack("<4sIII", b"MMEC", 3, len(key_data), len(payload))
    group = path / "fixture"
    group.mkdir()
    name = key_hash.hex() + (".mtc" if texture else ".fxb")
    (group / name).write_bytes(header + key_hash + payload_hash + (b"" if texture else key_data) + payload)


class HarnessTests(unittest.TestCase):
    def setUp(self):
        storage = tempfile.TemporaryDirectory()
        self.addCleanup(storage.cleanup)
        self.base = Path(storage.name)
        self.fixture = self.base / "synthetic-fixture"
        self.fixture.mkdir()
        for name in ("case.pmm", "shape.pmx", "look.fx", "unsupported.fx", "grid.bmp"):
            (self.fixture / name).write_bytes(f"schema stand-in for {name}\n".encode())
        self.expected = {
            "pmm": {"fixture": "case.pmm", "version": "0002", "result": "opened", "active_model_count": 1},
            "models": [{"fixture": "shape.pmx", "vertices": 6, "indices": 6, "materials": 2,
                        "semantic_sha256": digest_bytes(b"geometry with two material partitions")}],
            "effects": {"look.fx": "accepted", "unsupported.fx": "rejected"},
        }
        path = self.base / "expectations.json"
        load.write_json(path, self.expected)
        main_exe, abi_exe = [self.base / f"{name}.bin" for name in load.VARIANTS]
        main_exe.write_bytes(b"synthetic main executable fingerprint; no code")
        abi_exe.write_bytes(b"synthetic abi executable fingerprint; no code")
        self.args = SimpleNamespace(
            output_parent=self.base, fixture_dir=self.fixture, expectations=path, main_exe=main_exe,
            abi_exe=abi_exe, main_sha256=digest_bytes(main_exe.read_bytes()),
            abi_sha256=digest_bytes(abi_exe.read_bytes()), abi_source_sha256=digest_bytes(b"synthetic source"),
            pairs=2, mode="synthetic",
        )
        self.root, self.plan = load.load_run(load.prepare(self.args))
        self.args.observations = self.base / "observations.json"

    def sample(self, variant="main", pair=1):
        return load.sample_path(self.root, self.plan, variant, pair)

    def checkpoint(self, phase, point, variant="main", pair=1):
        return load.checkpoint(self.root, self.plan, variant, pair, phase, point)

    def record_all(self, *, mismatch=None, missing=False, no_texture=False, no_cache=False, same_bad_process=False):
        records = []
        for side, variant in enumerate(load.VARIANTS):
            for pair in range(1, 3):
                sample = self.sample(variant, pair)
                for step, phase in enumerate(load.PHASES):
                    self.checkpoint(phase, "before", variant, pair)
                    if step == 0 and not (no_cache and side):
                        write_synthetic_cache(sample / "cache" / "MME", texture=False)
                        if not (no_texture and side):
                            write_synthetic_cache(sample / "cache" / "Textures", texture=True)
                    self.checkpoint(phase, "after", variant, pair)
                    outcome = json.loads(json.dumps(self.expected))
                    if side and pair == 1 and step == 1 and mismatch:
                        mismatch(outcome)
                    time = 10 * pair + 2 * step
                    delta = 3 if pair == 1 else -5
                    record = {
                        "variant": variant, "pair": pair, "phase": phase,
                        "exe_sha256": self.plan["builds"][variant]["exe"]["sha256"],
                        "process_session": f"{10 * side + 2 * (pair - 1) + (0 if step == 0 else 1):032x}",
                        "elapsed_ns": {k: time + (delta if side else 0) for k in load.METRICS},
                        "outcome": outcome,
                    }
                    record["elapsed_ns"]["texture_prepare"] = None if missing and not side else time - 5
                    if same_bad_process and side and pair == 1 and step == 2:
                        record["process_session"] = "f" * 32
                    records.append(record)
        report = {k: self.plan[k] for k in ("schema", "run_id", "mode", "fixture_sha256")}
        report["capture"] = {"host_label": "synthetic-linux", "adapter_sha256": digest_bytes(b"test adapter"),
                             "timing_clock": "monotonic_in_process"}
        report["records"] = records
        load.write_json(self.args.observations, report)
        return report

    def comparison(self):
        path, status = load.compare(self.root, self.plan, self.args)
        return json.loads((path / "report.json").read_text()), path, status

    def test_pins_fixture_and_paths(self):
        paths = load.adapter_paths(self.root, self.plan, "abi", 2)
        self.assertEqual(Path(paths["fixture_dir"]), self.root / "fixture")
        self.assertEqual(Path(paths["env"]["MMS_EFFECT_CACHE_DIR"]), self.sample("abi", 2) / "cache/MME")
        self.assertEqual(self.plan["builds"]["main"]["commit"], load.MAIN)
        self.assertEqual(self.plan["builds"]["abi"]["commit"], load.ABI)
        self.assertEqual(self.plan["fixture"]["shape.pmx"]["sha256"],
                         digest_bytes((self.fixture / "shape.pmx").read_bytes()))
        self.assertNotIn(str(self.base), (self.root / "plan.json").read_text())
        self.assertEqual(len({self.sample(v, pair) for v in load.VARIANTS for pair in range(1, 3)}), 4)

    def test_successful_paired_comparison_and_csv(self):
        self.record_all()
        report, path, status = self.comparison()
        self.assertEqual(status, 0)
        self.assertEqual(report["semantic_verdict"], "observations_match_fixture")
        self.assertEqual(report["performance_verdict"], "not_evaluated")
        self.assertEqual(report["evidence_origin"], "synthetic")
        self.assertEqual(report["cache_hit_verdict"], "not_evaluated")
        self.assertEqual(len(report["observations"]), 12)
        self.assertEqual(report["timing"][load.PHASES[1]]["total_ready"], {
            "abi_median_ns": 16, "main_median_ns": 17, "median_paired_abi_minus_main_ns": -1,
            "pairs_compared": 2, "pairs_excluded": {
                "disk_payload_absent": 0, "missing_telemetry": 0, "outcome_mismatch": 0}})
        with (path / "observations.csv").open(newline="", encoding="utf-8") as file:
            rows = list(csv.DictReader(file))
        self.assertEqual(len(rows), 12)
        self.assertEqual(rows[1]["before_effect_fxb"], "1")
        self.assertEqual(rows[0]["before_effect_fxb"], "0")
        self.assertEqual(rows[0]["mode"], "synthetic")
        self.assertNotIn(str(self.base), (path / "report.json").read_text())
        self.assertNotEqual(self.comparison()[1], path)

    def test_exact_outcome_differences_and_timing_gate(self):
        def change(outcome):
            outcome["pmm"]["result"] = "rejected"
            outcome["models"][0]["materials"] = 3
            outcome["models"][0]["semantic_sha256"] = "a" * 64
            outcome["effects"]["look.fx"] = "rejected"
        self.record_all(mismatch=change)
        report, _, code = self.comparison()
        self.assertEqual(code, 1)
        self.assertEqual(
            {item["field"] for item in report["differences"]},
            {"outcome.effects.look.fx", "outcome.models[0].materials", "outcome.models[0].semantic_sha256",
             "outcome.pmm.result"})
        self.assertEqual(report["timing"][load.PHASES[1]]["total_ready"]["pairs_excluded"]["outcome_mismatch"], 1)

    def test_null_missing_telemetry_not_credited_or_written_as_zero(self):
        self.record_all(missing=True)
        report, path, status = self.comparison()
        self.assertEqual(status, 0)
        stage = report["timing"][load.PHASES[1]]["texture_prepare"]
        self.assertEqual(stage["pairs_compared"], 0)
        self.assertEqual(stage["pairs_excluded"]["missing_telemetry"], 2)
        self.assertIsNone(stage["main_median_ns"])
        with (path / "observations.csv").open(newline="", encoding="utf-8") as file:
            self.assertEqual(next(csv.DictReader(file))["texture_prepare_ns"], "")

    def test_effect_disk_artifact_does_not_prove_texture_disk_warm(self):
        self.record_all(no_texture=True)
        report, _, status = self.comparison()
        self.assertEqual(status, 0)
        warm = report["timing"][load.PHASES[1]]
        self.assertEqual(warm["texture_prepare"]["pairs_excluded"]["disk_payload_absent"], 2)
        self.assertEqual(warm["effect_accept"]["pairs_compared"], 2)

    def test_no_persisted_payload_disqualifies_new_process_disk_phase(self):
        self.record_all(no_cache=True)
        report, _, status = self.comparison()
        self.assertEqual(status, 0)
        self.assertEqual(report["timing"][load.PHASES[1]]["total_ready"]["pairs_excluded"]["disk_payload_absent"], 2)
        self.assertEqual(report["timing"][load.PHASES[2]]["total_ready"]["pairs_compared"], 2)

    def test_reject_false_same_process(self):
        self.record_all(same_bad_process=True)
        with self.assertRaisesRegex(load.Invalid, "process sessions"):
            self.comparison()

    def test_reject_primed_cold_root_without_deleting_foreign_file(self):
        marker = self.sample() / "cache" / "do-not-delete.txt"
        marker.write_bytes(b"foreign marker")
        with self.assertRaisesRegex(load.Invalid, "cold checkpoint already"):
            self.checkpoint(load.PHASES[0], "before")
        self.assertEqual(marker.read_bytes(), b"foreign marker")
        self.assertEqual(list((self.sample() / "checkpoints").iterdir()), [])

    def test_reject_injected_cache_between_operations(self):
        self.checkpoint(load.PHASES[0], "before")
        self.checkpoint(load.PHASES[0], "after")
        (self.sample() / "cache" / "foreign.bin").write_bytes(b"late write")
        with self.assertRaisesRegex(load.Invalid, "changed between operations"):
            self.checkpoint(load.PHASES[1], "before")

    def test_fake_payload_extensions_are_not_credited_as_disk_candidates(self):
        group = self.sample() / "cache" / "Textures" / "fake"
        group.mkdir()
        (group / ("a" * 64 + ".mtc")).write_bytes(b"not a cache header" * 5)
        (group / "metadata.txt").write_bytes(b"also not a cache payload")
        snapshot = load.cache_snapshot(self.sample())
        self.assertEqual(snapshot["file_count"], 2)
        self.assertEqual(snapshot["payload_files"], {"effect_fxb": 0, "texture_mtc": 0})

    def test_reject_mismatching_executable_after_prepare(self):
        self.record_all()
        self.args.main_exe.write_bytes(b"mutated")
        with self.assertRaisesRegex(load.Invalid, "executable SHA-256 mismatch"):
            self.comparison()

    def test_missing_or_changed_fixture_rejected(self):
        staged_model = self.root / "fixture" / "shape.pmx"
        staged_model.chmod(0o644)
        staged_model.write_bytes(b"unexpected")
        with self.assertRaisesRegex(load.Invalid, "fixture bytes/path set changed"):
            self.checkpoint(load.PHASES[0], "before")
        self.assertEqual((self.fixture / "shape.pmx").read_bytes(), b"schema stand-in for shape.pmx\n")

    def test_wrong_cold_sequence_and_repeated_checkpoint(self):
        with self.assertRaises(FileNotFoundError):
            self.checkpoint(load.PHASES[0], "after")
        self.checkpoint(load.PHASES[0], "before")
        with self.assertRaises(FileExistsError):
            self.checkpoint(load.PHASES[0], "before")

    def test_symlink_or_reparse_point_rejected(self):
        victim = self.base / "outside.bin"
        victim.write_bytes(b"untouched")
        try:
            (self.sample() / "cache" / "outside.bin").symlink_to(victim)
        except OSError as exc:
            self.skipTest(f"symlink creation unavailable: {exc}")
        with self.assertRaisesRegex(load.Invalid, "symlink or reparse"):
            self.checkpoint(load.PHASES[0], "before")
        self.assertEqual(victim.read_bytes(), b"untouched")

    def test_reject_many_unsafe_fixture_names(self):
        for value in ("../data.pmm", "/scene.pmm", "C:/scene.pmm", "secret:scene.pmm", "a/CON.pmx",
                      "a\\b.pmx", "a//b.pmx", ".hidden", "aux.png", "scene. ", "模型.pmx", "folder."):
            with self.subTest(value=value), self.assertRaises(load.Invalid):
                load.relative(value)
        self.assertEqual(load.relative("Scene-1/model_A.pmx"), "Scene-1/model_A.pmx")
        if os.name != "nt":
            (self.fixture / "CON.pmx").write_bytes(b"bad path")
            with self.assertRaisesRegex(load.Invalid, "unsafe relative path"):
                load.prepare(self.args)

    def test_application_mode_does_not_treat_hash_as_proof_it_is_pe(self):
        self.args.mode = "application"
        with self.assertRaisesRegex(load.Invalid, "Windows .exe"):
            load.prepare(self.args)

    def test_pmm_header_matches_declared_version(self):
        project = self.fixture / "case.pmm"
        project.write_bytes(b"Polygon Movie maker 0002\0" + bytes(5))
        self.assertIsNone(load.validate_pmm_header(self.fixture, self.expected))
        self.expected["pmm"]["version"] = "0003"
        with self.assertRaisesRegex(load.Invalid, "PMM header"):
            load.validate_pmm_header(self.fixture, self.expected)

    def test_reject_timing_report_without_clock(self):
        result = self.record_all()
        result["capture"]["timing_clock"] = "unavailable"
        self.args.observations = self.base / "clock.json"
        load.write_json(self.args.observations, result)
        with self.assertRaisesRegex(load.Invalid, "supported capture clock"):
            self.comparison()

    def test_reject_duplicate_json_keys_and_wrong_executable_pin_from_report(self):
        raw = self.base / "duplicate.json"
        raw.write_bytes(b'{"schema":1,"schema":1}')
        with self.assertRaisesRegex(load.Invalid, "duplicate key"):
            load.read_json(raw)
        result = self.record_all()
        result["records"][0]["exe_sha256"] = "0" * 64
        raw.write_text(json.dumps(result), encoding="utf-8")
        self.args.observations = raw
        with self.assertRaisesRegex(load.Invalid, "wrong executable"):
            self.comparison()

    def test_cli_reports_without_touching_existing_report_and_returns_exit_status(self):
        self.record_all()
        command = [sys.executable, "-B", str(Path(load.__file__).absolute()), "compare", str(self.root),
                   "--main-exe", str(self.args.main_exe), "--abi-exe", str(self.args.abi_exe),
                   "--observations", str(self.args.observations)]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(Path(result.stdout.strip(), "report.json").is_file())
        self.args.main_exe.write_bytes(b"mismatch")
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()) as stderr:
            self.assertEqual(load.main(command[3:]), 2)
        self.assertIn("SHA-256 mismatch", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
