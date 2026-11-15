"""End-to-end tests for the standalone mms_format_inspect binary."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from format_inspect_test_support import allocated_bytes, make_sparse

BINARY = Path(sys.argv.pop(1)).resolve()
PMM_MAGIC = b"Polygon Movie maker"
VMD_MAGIC = b"Vocaloid Motion Data "
MARKER = b"PMM3END\0"
FOOTER = MARKER + bytes(56)  # Intentionally invalid as a real container footer.


def pmm(version, separator=b" ", ending=b"\0" + bytes(5)):
    return PMM_MAGIC + separator + version + ending


def pmx(version, encoding, ignored=b" ", settings_size=8):
    return b"PMX" + ignored + struct.pack("<f", version) + bytes(
        [settings_size, encoding, 0, 1, 1, 1, 1, 1, 1]
    )


def vmd(version, ending=b"\0" + bytes(4)):
    return VMD_MAGIC + version + ending


def result(status, fmt=None, version=None, encoding=None, full="unknown",
           original="unknown", footer="not_checked", count=0):
    return dict(schema=1, status=status, format=fmt, version=version, encoding=encoding,
                native_header=dict(full=full, original=original), pmm3end=footer,
                bytes_examined=count, validation="header_only")


class FormatInspect(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)

    def run_cli(self, *arguments):
        return subprocess.run([BINARY, *arguments], cwd=self.folder, capture_output=True,
                              text=True, timeout=10, check=False)

    def check_json(self, name, code, expected):
        run = self.run_cli("--json", "--", name)
        self.assertEqual(run.returncode, code, run.stderr)
        self.assertEqual(run.stderr, "")
        self.assertEqual(len(run.stdout.splitlines()), 1)
        self.assertEqual(json.loads(run.stdout), expected)
        self.assertNotIn(name, run.stdout)

    def fixture(self, name, data, code, expected):
        path = self.folder / name
        path.write_bytes(data)
        before = path.stat()
        names = sorted(self.folder.iterdir())
        self.check_json(name, code, expected)
        after = path.stat()
        self.assertEqual(sorted(self.folder.iterdir()), names)
        self.assertEqual(path.read_bytes(), data)
        self.assertEqual((before.st_size, before.st_mtime_ns, before.st_mode),
                         (after.st_size, after.st_mtime_ns, after.st_mode))

    def test_pmm_profiles_by_content(self):
        for version, full, original in (
            ("0001", "accepted", "accepted"),
            ("0002", "accepted", "accepted"),
            ("0003", "accepted", "rejected"),
            ("other", "rejected", "rejected"),
        ):
            with self.subTest(version=version):
                wire_version = "0004" if version == "other" else version
                self.fixture(
                    "-非工程 sample.pmx", pmm(wire_version.encode(), b"!", b"\0noise"), 0,
                    result("recognized", "pmm", version, full=full, original=original, count=30),
                )
        self.fixture(
            "invalid-version-nul.pmm", pmm(b"0002", ending=b"!" + bytes(5)), 0,
            result("recognized", "pmm", "other", full="rejected", original="rejected", count=30),
        )

    def test_pmx_version_and_encoding(self):
        for number, encoding, version_label, encoding_label, gate in (
            (2.0, 0, "2.0", "utf-16le", "accepted"),
            (2.0, 1, "2.0", "utf-8", "rejected"),
            (2.0, 255, "2.0", "other", "rejected"),
            (2.1, 0, "2.1", "utf-16le", "rejected"),
            (2.1, 1, "2.1", "utf-8", "rejected"),
            (2.5, 0, "other", "utf-16le", "rejected"),
        ):
            with self.subTest(number=number, encoding=encoding):
                self.fixture(
                    "differs.vmd", pmx(number, encoding), 0,
                    result("recognized", "pmx", version_label, encoding_label, gate, gate, count=17),
                )
        self.fixture(
            "native-ignored-fields.dat", pmx(2.0, 0, b"?", 0), 0,
            result("recognized", "pmx", "2.0", "utf-16le", "accepted", "accepted", count=17),
        )

    def test_vmd_exact_signatures(self):
        for wire, version, gate in (
            (b"file", "1", "accepted"), (b"0002", "2", "accepted"), (b"0003", "other", "rejected"),
        ):
            with self.subTest(version=version):
                self.fixture(
                    "not-a-project.pmm", vmd(wire, b"\0junk"), 0,
                    result("recognized", "vmd", version, full=gate, original=gate, count=30),
                )
        self.fixture(
            "invalid-tag-nul.vmd", vmd(b"file", b"Xjunk"), 0,
            result("recognized", "vmd", "other", full="rejected", original="rejected", count=30),
        )

    def test_incomplete_and_unrecognized(self):
        for data, status, fmt in (
            (b"", "unrecognized", None),
            (b"arbitrary", "unrecognized", None),
            (b"pmx", "unrecognized", None),
            (FOOTER, "unrecognized", None),
            (b"P", "incomplete", None),
            (PMM_MAGIC[:-1], "incomplete", None),
            (pmm(b"0002")[:-1], "incomplete", "pmm"),
            (pmx(2.0, 1)[:-1], "incomplete", "pmx"),
            (b"Vocaloid Motion Data", "incomplete", None),
            (vmd(b"file")[:-1], "incomplete", "vmd"),
            (vmd(b"0002")[:-1], "incomplete", "vmd"),
        ):
            with self.subTest(data=data):
                self.fixture("truncated.pmm", data, 1, result(status, fmt, count=min(30, len(data))))

    def test_footer_marker_is_not_validation(self):
        for tail, marker, count in (
            (bytes(64), "not_detected", 94),
            (FOOTER, "marker_only", 94),
            (FOOTER[:-1], "not_checked", 30),
            (FOOTER + b"\0", "not_detected", 94),
            (FOOTER.replace(b"D\0", b"D!"), "not_detected", 94),
        ):
            with self.subTest(marker=marker, length=len(tail)):
                self.fixture(
                    "footer.vmd", pmm(b"0003") + tail, 0,
                    result("recognized", "pmm", "0003", full="accepted", original="rejected",
                           footer=marker, count=count),
                )
        self.fixture(
            "footer-wrong-wrapper.pmm", pmm(b"0002") + FOOTER, 0,
            result("recognized", "pmm", "0002", full="accepted", original="accepted", count=30),
        )

    def test_sparse_file_window_budget(self):
        path = self.folder / "sparse.pmx"
        size = (5 << 30) + 13
        header = pmm(b"0003")
        with path.open("wb") as stream:
            make_sparse(stream)
            stream.write(header)
            stream.seek(size - len(FOOTER))
            stream.write(FOOTER)
            # Finish fixture allocation before measuring read-only inspection.
            stream.flush()
            os.fsync(stream.fileno())
        before = path.stat()
        allocation = allocated_bytes(path)
        self.assertEqual(before.st_size, size)
        self.assertLess(allocation, size)
        self.check_json(
            path.name, 0, result("recognized", "pmm", "0003", full="accepted", original="rejected",
                                 footer="marker_only", count=94),
        )
        after = path.stat()
        self.assertEqual((after.st_size, after.st_mtime_ns, allocated_bytes(path)),
                         (before.st_size, before.st_mtime_ns, allocation))
        with path.open("rb") as stream:
            self.assertEqual(stream.read(30), header)
            stream.seek(-64, os.SEEK_END)
            self.assertEqual(stream.read(64), FOOTER)
        self.assertEqual(list(self.folder.iterdir()), [path])

    def test_usage_text_io_error_and_fifo(self):
        run = self.run_cli("--help")
        self.assertEqual(run.returncode, 0)
        self.assertIn("Usage:", run.stdout)
        self.assertEqual(self.run_cli().returncode, 2)
        self.assertEqual(self.run_cli("--bad").returncode, 2)
        self.assertEqual(self.run_cli("one.pmm", "two.pmx").returncode, 2)
        self.check_json("missing.pmm", 2, result("io_error"))
        (self.folder / "dir.pmm").mkdir()
        self.check_json("dir.pmm", 2, result("io_error"))
        if hasattr(os, "mkfifo"):
            os.mkfifo(self.folder / "fifo.pmm")
            self.check_json("fifo.pmm", 2, result("io_error"))

        name = "private-file-name.vmd"
        (self.folder / name).write_bytes(pmm(b"0003") + FOOTER)
        run = self.run_cli(name)
        self.assertEqual(run.returncode, 0)
        self.assertNotIn(name, run.stdout + run.stderr)
        self.assertIn("full=accepted, original=rejected", run.stdout)
        self.assertIn("standalone codec marker only (not verified)", run.stdout)
        self.assertIn("file contents not validated or imported", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
