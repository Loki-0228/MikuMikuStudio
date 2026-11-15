"""End-to-end PMX metadata cases, including fixtures from the real PMX writer."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
from pmxlib import PmxModel

BINARY = Path(sys.argv.pop(1)).resolve()
FIELDS = ("name_local", "name_en", "comment_local", "comment_en")
SECTIONS = ("vertices", "faces", "textures", "materials", "bones", "rigid_bodies", "joints")
HEADER_SIZE = 17
LIMIT = 64 * 1024


def wire_model(version=2.0, encoding=1, fields=("", "", "", "")):
    model = PmxModel()
    model.version, model.encoding = version, encoding
    for key, value in zip(FIELDS, fields):
        setattr(model, key, value)
    model.raw_sections = {key: bytes(4) for key in SECTIONS}
    if version == 2.1:
        model.raw_sections["soft_bodies"] = bytes(4)
    return model


def field_bytes(data):
    return struct.pack("<i", len(data)) + data


class PmxMetadata(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)

    def run_cli(self, *args):
        return subprocess.run([BINARY, *args], cwd=self.folder, capture_output=True,
                              timeout=10, check=False)

    def check_json(self, name, status, count, fields=None, header_status="recognized"):
        code = {"metadata_ok": 0, "io_error": 2, "budget_exceeded": 3}.get(status, 1)
        run = self.run_cli("--pmx-metadata", "--json", "--", name)
        self.assertEqual(run.returncode, code, run)
        self.assertEqual(run.stderr, b"")
        self.assertEqual(len(run.stdout.decode("utf-8").splitlines()), 1)
        document = json.loads(run.stdout)
        self.assertNotIn(name.encode(), run.stdout)
        self.assertNotIn(str(self.folder).encode(), run.stdout)
        self.assertEqual(document["schema"], 1)
        self.assertEqual(document["status"], header_status)
        self.assertEqual(document["bytes_examined"], count)
        self.assertEqual(document["validation"], "pmx_model_metadata_only")
        self.assertEqual(document["pmm3end"], "not_checked")
        metadata = {"status": status}
        if fields is not None:
            metadata.update(zip(FIELDS, fields))
        self.assertEqual(document["pmx_metadata"], metadata)
        return document

    def fixture(self, data, status, count, fields=None, header_status="recognized"):
        name = "private_source_model.vmd"
        (self.folder / name).write_bytes(data)
        return self.check_json(name, status, count, fields, header_status)

    def test_writer_bilingual_models_and_native_gates(self):
        cases = (
            (2.0, 0, ("初音ミク 🌟", "", "署名：模型测试\nCopyright fixture", "Line \x00 after NUL")),
            (2.0, 1, ("数据参考 𠀀", "UTF-8 example", "説明 🧊", 'line\nquote " and \\')),
            (2.1, 0, ("別のモデル 🌙", "Another model", "説明", "")),
            (2.1, 1, ("2.1 model 🎵", "", "unsupported to load", "metadata still inspectable")),
        )
        for version, encoding, values in cases:
            with self.subTest(version=version, encoding=encoding):
                model = wire_model(version, encoding, values)
                path = self.folder / "-fixture.vmd"
                model.save(path)
                data = path.read_bytes()
                decoded = PmxModel.from_file(path)
                self.assertEqual(tuple(getattr(decoded, key) for key in FIELDS), values)
                end = HEADER_SIZE + sum(4 + len(value.encode(model.codec)) for value in values)
                self.assertGreater(len(data), end)
                before = path.stat()
                mode = self.check_json(path.name, "metadata_ok", end, values)
                self.assertEqual(mode["format"], "pmx")
                self.assertEqual(mode["version"], str(version))
                self.assertEqual(mode["encoding"], "utf-16le" if encoding == 0 else "utf-8")
                gate = "accepted" if (version, encoding) == (2.0, 0) else "rejected"
                self.assertEqual(mode["native_header"], {"full": gate, "original": gate})
                default = self.run_cli("--json", "--", path.name)
                self.assertEqual(default.returncode, 0, default)
                default_fields = json.loads(default.stdout)
                self.assertNotIn("pmx_metadata", default_fields)
                self.assertEqual(default_fields["native_header"], mode["native_header"])
                self.assertEqual(path.read_bytes(), data)
                after = path.stat()
                self.assertEqual((before.st_size, before.st_mode, before.st_mtime_ns),
                                 (after.st_size, after.st_mode, after.st_mtime_ns))
                self.assertEqual(list(self.folder.iterdir()), [path])

    def test_controls_and_unicode_boundaries_round_trip(self):
        controls = "\x00\t\n\x0b\x0c\r\x1b\x1c\x7f\x85\x9b\x9d\u061c\u200e\u200f\u2028\u2029"
        controls += "\u202a\u202b\u202c\u202d\u202e\u2066\u2067\u2068\u2069\u206f\ufeff"
        boundary = "\x7f\u0080\u07ff\u0800\ud7ff\ue000\uffff\U00010000\U0010ffff"
        model_fields = ('before " \\ ' + controls + " after", boundary, "No normalization: e\u0301", "🧊")
        for encoding in (0, 1):
            with self.subTest(encoding=encoding):
                model = wire_model(encoding=encoding, fields=model_fields)
                path = self.folder / "untrusted.pmx"
                model.save(path)
                end = HEADER_SIZE + sum(4 + len(value.encode(model.codec)) for value in model_fields)
                self.check_json(path.name, "metadata_ok", end, model_fields)
                plain = self.run_cli("--pmx-metadata", path.name)
                structured = self.run_cli("--pmx-metadata", "--json", path.name)
                self.assertEqual(plain.returncode, 0, plain)
                self.assertEqual(plain.stderr, b"")
                self.assertNotIn(path.name.encode(), plain.stdout)
                for ch in controls:
                    for run in (plain, structured):
                        self.assertIn(f"\\u{ord(ch):04x}".encode(), run.stdout)
                        if ch != "\n":
                            self.assertNotIn(ch.encode(), run.stdout)
                for expected, key in zip(model_fields, FIELDS):
                    marker = f"  {key}: "
                    text = plain.stdout.decode("utf-8")
                    value_line, _ = text.split(marker, 1)[1].split("\n", 1)
                    self.assertEqual(json.loads(value_line), expected)
                self.assertIn(b"model body not read or imported", plain.stdout)

    def test_empty_fields_and_no_body(self):
        model = wire_model()
        data = model.to_bytes()[:HEADER_SIZE + 16]
        self.fixture(data, "metadata_ok", len(data), ("", "", "", ""))

    def test_absence_and_strict_wire_admission(self):
        self.fixture(b"", "absent", 0, header_status="unrecognized")
        self.fixture(b"unknown" + bytes(100), "absent", 30, header_status="unrecognized")
        self.fixture(b"Polygon Movie maker 0003\0" + bytes(5) +
                     b"PMM3END\0" + bytes(56), "absent", 30)
        header = bytearray(wire_model(encoding=0).to_bytes()[:HEADER_SIZE])
        for index, value, status in [
            (3, 0, "invalid"), (8, 0, "invalid"), (8, 7, "invalid"),
            (8, 9, "unsupported"), (9, 2, "unsupported"), (10, 5, "invalid"),
            *((index, 3, "invalid") for index in range(11, 17)),
        ]:
            with self.subTest(index=index, value=value):
                malformed = header.copy()
                malformed[index] = value
                result = self.fixture(malformed + bytes(100), status, HEADER_SIZE)
                self.assertEqual(result["native_header"]["full"], "rejected" if index == 9 else "accepted")
        self.fixture(wire_model(version=2.5).to_bytes(), "unsupported", HEADER_SIZE)
        for end in (3, 4, 9, 16):
            with self.subTest(end=end):
                self.fixture(header[:end], "truncated", end, header_status="incomplete")

    def test_signed_byte_lengths_and_truncation(self):
        header = wire_model().to_bytes()[:HEADER_SIZE]
        start = header + field_bytes(b"not released on error")
        self.fixture(header, "truncated", HEADER_SIZE)
        self.fixture(header + b"\x00\x00", "truncated", HEADER_SIZE)
        self.fixture(header + struct.pack("<i", 4) + b"123", "truncated", HEADER_SIZE + 4)
        for value in (-1, -2147483648):
            self.fixture(start + struct.pack("<i", value) + bytes(20), "invalid", len(start) + 4)
        for value in (LIMIT + 1, 0x7fffffff):
            self.fixture(start + struct.pack("<i", value), "budget_exceeded", len(start) + 4)
        self.fixture(start + bytes(8) + b"\x00\x00\x00", "truncated", len(start) + 8)

    def test_malformed_unicode_fails_without_partial_output(self):
        malformed = {
            1: (b"\xc0\x80", b"\xc1\xbf", b"\x80", b"\xef\xbb", b"\xe0\x80\xaf", b"\xf0\x8f\xbf\xbf",
                b"\xc2\x20", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80", b"\xff"),
            0: (b"X", b"\x00\xd8", b"\x00\xdc", b"\x00\xd8A\x00", b"\x00\xdc\x00\xd8"),
        }
        for encoding, cases in malformed.items():
            model = wire_model(encoding=encoding)
            start = model.to_bytes()[:HEADER_SIZE] + model.encode_text("not released")
            for invalid in cases:
                with self.subTest(encoding=encoding, invalid=invalid):
                    count = len(start) + 4 + (len(invalid) if encoding == 1 or len(invalid) % 2 == 0 else 0)
                    self.fixture(start + field_bytes(invalid) + bytes(8), "invalid", count)

    def test_exact_limit_and_sparse_model_body(self):
        values = ("A" * LIMIT, "B" * LIMIT, "C" * LIMIT, "D" * LIMIT)
        model = wire_model(fields=values)
        path = self.folder / "sparse-mismatch.pmm"
        end = HEADER_SIZE + 16 + 4 * LIMIT
        tail_size = (5 << 30) + 71
        with path.open("wb") as sparse:
            sparse.write(model.to_bytes()[:end])
            sparse.write(b"\xff\xff\xff\xff")  # Invalid, unread vertex count.
            sparse.seek(tail_size - 1)
            sparse.write(b"X")
        before = path.stat()
        self.assertEqual(before.st_size, tail_size)
        self.check_json(path.name, "metadata_ok", end, values)
        after = path.stat()
        self.assertEqual((before.st_size, before.st_blocks, before.st_mode, before.st_mtime_ns),
                         (after.st_size, after.st_blocks, after.st_mode, after.st_mtime_ns))
        self.assertEqual(list(self.folder.iterdir()), [path])
        with path.open("rb") as model_file:
            self.assertEqual(model_file.read(end + 4),
                             model.to_bytes()[:end] + b"\xff\xff\xff\xff")
            model_file.seek(-1, os.SEEK_END)
            self.assertEqual(model_file.read(), b"X")

    def test_io_and_option_isolation(self):
        help_run = self.run_cli("--pmx-metadata", "--help")
        self.assertEqual(help_run.returncode, 0, help_run)
        self.assertIn(b"--pmx-metadata", help_run.stdout)
        for flags in (
            ("--pmx-metadata",),
            ("--pmm-directory", "--pmx-metadata", "x"),
            ("--pmx-metadata", "--pmm-verify-payloads", "x"),
            ("--pmx-metadata", "--pmx-metadata", "x"),
        ):
            run = self.run_cli(*flags)
            self.assertEqual(run.returncode, 2, run)
            self.assertEqual(run.stdout, b"")
            self.assertTrue(run.stderr.startswith(b"Usage:"))
        for name in ("missing", "subdir", "named_pipe", "linked_source.pmx"):
            if name == "subdir":
                (self.folder / name).mkdir()
            if name == "named_pipe":
                os.mkfifo(self.folder / name)
            if name == "linked_source.pmx":
                target = self.folder / "link_target.pmx"
                wire_model().save(target)
                (self.folder / name).symlink_to(target)
                self.assertEqual(self.run_cli(name).returncode, 0)
            self.check_json(name, "io_error", 0, header_status="io_error")


if __name__ == "__main__":
    unittest.main(verbosity=2)
