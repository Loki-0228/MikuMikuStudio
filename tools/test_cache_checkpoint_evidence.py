#!/usr/bin/env python3
"""Run with python -B tools/test_cache_checkpoint_evidence.py --decoder /path/to/decode_checkpoint."""
import argparse
import errno
import itertools
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

import cache_checkpoint_evidence as evidence


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux checkpoint evidence tool")
class CheckpointBatchTests(unittest.TestCase):
    decoder = None

    def setUp(self):
        if self.decoder is None:
            self.skipTest("run the test script with --decoder")
        folder = tempfile.TemporaryDirectory(prefix="mms-checkpoint-batch-")
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.evidence = self.root / "preserved"
        self.evidence.mkdir()
        self.valid = self.write_blob(20)
        self.torn = self.write_blob(21, limit=evidence.HEADER + 256)
        self.missing = self.evidence / f"{22:032x}.blob"

    def write_blob(self, revision, *, limit=None):
        path = self.evidence / f"{revision:032x}.blob"
        data = evidence.synthetic_blob(path, evidence.synthetic_payload(revision))
        path.write_bytes(data[:limit])
        return path

    def fixture_snapshot(self):
        rows = {}
        for path in self.evidence.rglob("*"):
            info = path.lstat()
            content = None
            if path.is_symlink():
                content = os.readlink(path)
            elif stat.S_ISREG(info.st_mode) and info.st_mode & stat.S_IRUSR:
                content = evidence.sha(path.read_bytes())
            rows[path.relative_to(self.evidence)] = (info.st_mode, info.st_nlink, evidence.file_identity(info), content)
        return rows

    def run_cli(self, code, *paths, decoder=None):
        before = self.fixture_snapshot()
        run = subprocess.run([sys.executable, "-B", evidence.__file__, "inspect", "--decoder",
                              str(self.decoder if decoder is None else decoder), *map(str, paths)],
                             capture_output=True, text=True, timeout=evidence.TIMEOUT + 5)
        self.assertEqual(self.fixture_snapshot(), before)
        self.assertEqual(run.returncode, code, run.stderr)
        self.assertEqual(run.stderr, "")
        self.assertNotIn(str(self.root), run.stdout)
        rows = json.loads(run.stdout)
        self.assertEqual(len(rows), len(paths))
        self.assertEqual([row["file"] for row in rows],
                         [path.name if evidence.NAME.fullmatch(path.name) else None for path in paths])
        self.assertTrue(all(row["scope"] == evidence.SCOPE for row in rows))
        return rows

    def test_valid_and_torn_rows_survive_missing_input_in_all_positions(self):
        valid, = self.run_cli(0, self.valid)
        torn, = self.run_cli(1, self.torn)
        self.assertEqual(valid["status"], "checkpoint_codec_ok")
        self.assertEqual(valid["decoded"]["revision"], 20)
        self.assertEqual(torn["status"], "short_payload")
        self.assertEqual(torn["sha256"], evidence.sha(self.torn.read_bytes()).hex())
        self.assertEqual(self.run_cli(1, self.torn, self.valid), [torn, valid])
        missing = dict(file=self.missing.name, scope=evidence.SCOPE,
                       status="evidence_unavailable", error="file_missing")
        expected = {self.valid: valid, self.torn: torn, self.missing: missing}
        for paths in itertools.permutations(expected):
            with self.subTest(paths=[path.name for path in paths]):
                self.assertEqual(self.run_cli(2, *paths), [expected[path] for path in paths])
        self.assertEqual(self.run_cli(2, self.missing), [missing])

    def test_unreadable_input_does_not_hide_other_observations(self):
        unreadable = self.write_blob(23)
        original = unreadable.read_bytes()
        unreadable.chmod(0)
        try:
            if os.access(unreadable, os.R_OK):
                self.skipTest("permission denial needs an unprivileged test process")
            valid, error, torn = self.run_cli(2, self.valid, unreadable, self.torn)
            self.assertEqual(valid["status"], "checkpoint_codec_ok")
            self.assertEqual(error["status"], "evidence_unavailable")
            self.assertEqual(error["error"], "file_unreadable")
            self.assertEqual(error["errno"], errno.EACCES)
            self.assertEqual(torn["status"], "short_payload")
        finally:
            unreadable.chmod(0o600)
            self.assertEqual(unreadable.read_bytes(), original)

    def test_unsafe_inputs_have_sanitized_per_file_reasons(self):
        link = self.evidence / f"{24:032x}.blob"
        link.symlink_to(self.valid)
        parent_link = self.root / "parent-link"
        parent_link.symlink_to(self.evidence, target_is_directory=True)
        invalid = self.evidence / "unreportable\nfilename"
        invalid_row, link_row, parent_row, valid = self.run_cli(
            2, invalid, link, parent_link / self.valid.name, self.valid)
        self.assertEqual([row["error"] for row in (invalid_row, link_row, parent_row)],
                         ["invalid_filename", "unsafe_file", "unsafe_parent"])
        self.assertTrue(all(row["status"] == "evidence_unavailable" for row in (invalid_row, link_row, parent_row)))
        self.assertNotIn("unreportable", json.dumps(invalid_row))
        self.assertEqual(valid["status"], "checkpoint_codec_ok")

    def test_oversized_input_keeps_size_but_has_no_full_digest(self):
        oversized = self.write_blob(25)
        with oversized.open("rb+") as stream:
            stream.truncate(evidence.READ_MAX + 100)
        big, valid, missing, torn = self.run_cli(2, oversized, self.valid, self.missing, self.torn)
        self.assertEqual(big["status"], "trailing_bytes")
        self.assertEqual(big["bytes"], evidence.READ_MAX + 100)
        self.assertEqual(big["declared_payload_bytes"], len(evidence.synthetic_payload(25)))
        self.assertNotIn("sha256", big)
        self.assertEqual([row["status"] for row in (valid, missing, torn)],
                         ["checkpoint_codec_ok", "evidence_unavailable", "short_payload"])

    def test_bad_decoder_launch_only_affects_verified_payloads(self):
        missing_decoder = self.root / "no-helper"
        torn, error = self.run_cli(2, self.torn, self.valid, decoder=missing_decoder)
        self.assertEqual(torn["status"], "short_payload")
        self.assertEqual(error["status"], "checkpoint_codec_unavailable")
        self.assertEqual(error["error"], "decoder_launch_failed")
        self.assertEqual(error["sha256"], evidence.sha(self.valid.read_bytes()).hex())
        self.assertEqual(error["declared_payload_bytes"], len(evidence.synthetic_payload(20)))
        self.assertEqual(self.run_cli(1, self.torn, decoder=missing_decoder), [torn])

    def test_decoder_failure_modes_do_not_hide_success_or_torn_evidence(self):
        helper = self.root / "faulting-helper"
        helper.write_text(
            "#!/usr/bin/env python3\n"
            "import json, subprocess, sys, time\n"
            "data = sys.stdin.buffer.read()\n"
            "rev = int.from_bytes(data[24:32], 'little')\n"
            f"if rev == 26: print({str(self.root)!r}, file=sys.stderr); sys.exit(2)\n"
            f"if rev == 27: print({str(self.root)!r}); sys.exit(0)\n"
            "if rev == 28: print('null'); sys.exit(0)\n"
            f"if rev == 29: print('X' * {evidence.DECODER_JSON_MAX + 1}); sys.exit(0)\n"
            "if rev == 30:\n"
            f"    response = json.loads(subprocess.run([{str(self.decoder)!r}], input=data, capture_output=True).stdout)\n"
            f"    response['previous']['name'] = {str(self.root)!r}\n"
            "    print(json.dumps(response)); sys.exit(0)\n"
            f"if rev == 31: time.sleep({evidence.TIMEOUT + 10}); sys.exit(0)\n"
            f"sys.exit(subprocess.run([{str(self.decoder)!r}], input=data).returncode)\n"
        )
        helper.chmod(0o700)
        faulty = [self.write_blob(revision) for revision in range(26, 32)]
        valid, torn = self.run_cli(1, self.valid, self.torn)
        failed, = self.run_cli(2, faulty[0], decoder=helper)
        self.assertEqual(failed["returncode"], 2)
        self.assertEqual(failed["error"], "decoder_failed")
        for i in range(3):
            paths, expected = [self.valid, self.torn], [valid, torn]
            paths.insert(i, faulty[0])
            expected.insert(i, failed)
            with self.subTest(position=i):
                self.assertEqual(self.run_cli(2, *paths, decoder=helper), expected)
        observed = self.run_cli(2, self.valid, *faulty[1:], self.torn, decoder=helper)
        self.assertEqual(observed[0], valid)
        self.assertEqual(observed[-1], torn)
        for path, row, reason in zip(faulty[1:], observed[1:-1], ["invalid_decoder_output"] * 4 + ["decoder_timeout"]):
            self.assertEqual(row["status"], "checkpoint_codec_unavailable")
            self.assertEqual(row["error"], reason)
            self.assertEqual(row["sha256"], evidence.sha(path.read_bytes()).hex())
            self.assertNotIn("decoded", row)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--decoder", required=True, type=Path)
    args, test_args = parser.parse_known_args()
    CheckpointBatchTests.decoder = args.decoder.resolve(strict=True)
    unittest.main(argv=[sys.argv[0], *test_args])
