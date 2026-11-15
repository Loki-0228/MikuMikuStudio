"""Container metadata regression tests. The good fixture uses the shipping writer."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from format_inspect_test_support import allocated_bytes, make_sparse

BINARY = Path(sys.argv.pop(1)).resolve()
FIXTURE_TOOL = Path(sys.argv.pop(1)).resolve()

PREFIX = b"Polygon Movie maker 0003" + bytes(6)
WRAPPER_BYTES = 94
BLOCK_HEADER = struct.Struct("<IIIIQII")
TOC_HEADER = struct.Struct("<8sIIII")
TOC_ENTRY = struct.Struct("<IIIIQQII")
FOOTER = struct.Struct("<8sIIQQQQIIQ")


def toc_bytes(rows):
    return TOC_HEADER.pack(b"PMM3TOC\0", 1, 0, len(rows), 0) + b"".join(rows)


def footer_bytes(toc, toc_offset):
    return FOOTER.pack(b"PMM3END\0", 1, 0, len(PREFIX), toc_offset, len(toc),
                       toc_offset + len(toc) + FOOTER.size, zlib.crc32(toc), 0, 0)


def row(key, offset, payload_size=0, crc=0):
    return TOC_ENTRY.pack(len(key), 1, 0, 0, offset, payload_size, crc, 0) + key


def overwrite(data, offset, layout, value):
    return data[:offset] + struct.pack(layout, value) + data[offset + struct.calcsize(layout):]


class PmmDirectory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        fixture = Path(temporary.name) / "writer-output.pmm"
        subprocess.run([FIXTURE_TOOL, fixture], timeout=10, check=True)
        cls.good = fixture.read_bytes()
        toc_offset = FOOTER.unpack(cls.good[-FOOTER.size:])[4]
        cls.before_toc = cls.good[:toc_offset]
        cls.toc = cls.good[toc_offset:-FOOTER.size]
        cls.rows = []
        pos = TOC_HEADER.size
        while pos < len(cls.toc):
            key_size = struct.unpack_from("<I", cls.toc, pos)[0]
            next_pos = pos + TOC_ENTRY.size + key_size
            cls.rows.append(cls.toc[pos:next_pos])
            pos = next_pos
        cls.offsets = [TOC_ENTRY.unpack_from(r)[4] for r in cls.rows]

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "-not-to-echo-非工程.bin"

    def run_cli(self, *args):
        return subprocess.run([BINARY, *args], cwd=self.path.parent, capture_output=True,
                              text=True, timeout=10, check=False)

    def inspect(self, code, state, count=None):
        run = self.run_cli("--json", "--pmm-directory", "--", self.path.name)
        self.assertEqual(run.returncode, code, (run.stdout, run.stderr))
        self.assertEqual(run.stderr, "")
        self.assertEqual(len(run.stdout.splitlines()), 1)
        self.assertNotIn(self.path.name, run.stdout)
        self.assertNotIn(str(self.path), run.stdout)
        data = json.loads(run.stdout)
        self.assertEqual(data["validation"], "pmm_directory_metadata_only")
        self.assertEqual(data["pmm_directory"]["status"], state)
        if state != "metadata_ok":
            self.assertEqual(data["pmm_directory"]["blocks"], [])
        if count is not None:
            self.assertEqual(data["bytes_examined"], count)
        self.assertLessEqual(data["bytes_examined"], 1024 * 1024)
        return data

    def with_toc(self, toc, before=None):
        if before is None:
            before = self.before_toc
        return before + toc + footer_bytes(toc, len(before))

    def test_writer_and_payload_only_corruption(self):
        self.path.write_bytes(self.good)
        before = self.path.stat()
        data = self.inspect(0, "metadata_ok", WRAPPER_BYTES + len(self.toc) +
                            sum(BLOCK_HEADER.size + len(r) - TOC_ENTRY.size for r in self.rows))
        self.assertEqual(data["pmm_directory"]["blocks"], [
            dict(key_escaped="extension.geometry", version=7, flags=4, encoding=0,
                 payload_bytes=2 * 1024 * 1024),
            dict(key_escaped=r"opaque.\x85\x1b\x22\x5c\x0a", version=123, flags=8,
                 encoding=777, payload_bytes=3),
        ])
        text = self.run_cli("--pmm-directory", "--", self.path.name)
        self.assertEqual(text.returncode, 0)
        self.assertIn(r"opaque.\x85\x1b\x22\x5c\x0a", text.stdout)
        self.assertNotIn("\x1b", text.stdout)
        self.assertNotIn(self.path.name, text.stdout + text.stderr)
        self.assertIn("payloads and their CRCs not checked", text.stdout)
        after = self.path.stat()
        self.assertEqual((before.st_mtime_ns, before.st_size, before.st_mode),
                         (after.st_mtime_ns, after.st_size, after.st_mode))
        self.assertEqual(self.good, self.path.read_bytes())
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

        payload = self.offsets[0] + BLOCK_HEADER.size + len(b"extension.geometry")
        self.path.write_bytes(overwrite(self.good, payload, "<B", 0x34))
        self.assertEqual(self.inspect(0, "metadata_ok"), data)
        native = subprocess.run([FIXTURE_TOOL, "--check-codec", self.path], timeout=10, check=False)
        self.assertEqual(native.returncode, 1)

    def test_absent_and_io_errors(self):
        for data, code, count in (
            (PREFIX, 0, 30),
            (PREFIX + bytes(64), 0, 94),
            (self.good[:20] + b"0002" + self.good[24:], 0, 30),
            (b"Vocaloid Motion Data 0002" + bytes(6), 0, 30),
            (b"nothing", 1, 7),
        ):
            with self.subTest(data=data[:30]):
                self.path.write_bytes(data)
                self.inspect(code, "absent", count)
        self.path.unlink()
        self.assertEqual(self.inspect(2, "io_error", 0)["status"], "io_error")
        self.path.mkdir()
        self.inspect(2, "io_error", 0)
        self.assertEqual(self.run_cli("--pmm-directory", "--help").returncode, 0)
        self.assertEqual(self.run_cli("--pmm-directory", "--pmm-directory").returncode, 2)

    def test_footer_bounds_and_directory_crc_or_header(self):
        for pos, layout, value in (
            (8, "<I", 2), (12, "<I", 1), (16, "<Q", 29), (16, "<Q", 2**64 - 1),
            (24, "<Q", 29), (24, "<Q", 2**64 - 1), (32, "<Q", 23), (32, "<Q", 2**64 - 1),
            (40, "<Q", 123), (52, "<I", 1), (56, "<Q", 1),
        ):
            with self.subTest(footer_field=pos, value=value):
                self.path.write_bytes(overwrite(self.good, len(self.good) - FOOTER.size + pos,
                                                layout, value))
                self.inspect(1, "invalid", WRAPPER_BYTES)
        for pos, fmt, value in ((0, "<B", 0), (8, "<I", 2), (12, "<I", 1),
                                (16, "<I", 0), (16, "<I", 2**32 - 1), (20, "<I", 1)):
            with self.subTest(toc_field=pos, value=value):
                self.path.write_bytes(self.with_toc(overwrite(self.toc, pos, fmt, value)))
                self.inspect(1, "invalid", WRAPPER_BYTES + TOC_HEADER.size)
        self.path.write_bytes(overwrite(self.good, len(self.good) - FOOTER.size - 1, "<B", 0))
        self.inspect(1, "invalid", WRAPPER_BYTES + len(self.toc))

    def test_toc_exact_rows_unique_keys_and_block_bounds(self):
        first, second = self.rows
        for rows in (
            [first, second[:TOC_ENTRY.size] + b"\0" + second[TOC_ENTRY.size + 1:]],
            [first, overwrite(second[:TOC_ENTRY.size], 0, "<I", len(first) - TOC_ENTRY.size) +
             first[TOC_ENTRY.size:]],
            [first, overwrite(second, 0, "<I", 0)],
            [first, overwrite(second, 0, "<I", 256)],
            [first, overwrite(second, 16, "<Q", len(PREFIX) - 1)],
            [first, overwrite(second, 16, "<Q", len(self.before_toc) - 1)],
            [first, overwrite(second, 24, "<Q", 2**64 - 1)],
            [first, overwrite(second, 36, "<I", 1)],
            [first, second + b"x"],
        ):
            with self.subTest(row=rows[1]):
                toc = toc_bytes(rows)
                self.path.write_bytes(self.with_toc(toc))
                self.inspect(1, "invalid", WRAPPER_BYTES + len(toc))

    def test_block_header_and_key_checked_before_output(self):
        first, second = self.offsets
        for pos, fmt, value in ((0, "<I", 1), (4, "<I", 1), (8, "<I", 1), (12, "<I", 1),
                                (16, "<Q", 1), (24, "<I", 1), (28, "<I", 1)):
            with self.subTest(block_field=pos):
                self.path.write_bytes(overwrite(self.good, first + pos, fmt, value))
                self.inspect(1, "invalid", WRAPPER_BYTES + len(self.toc) + BLOCK_HEADER.size)
        self.path.write_bytes(overwrite(self.good, second + BLOCK_HEADER.size, "<B", ord("X")))
        self.inspect(1, "invalid")

    def test_native_allows_gaps_and_toc_order_but_overlap_is_unverified(self):
        gap = bytes(11)
        shifted = [overwrite(r, 16, "<Q", off + len(gap)) for r, off in zip(self.rows, self.offsets)]
        before = PREFIX + gap + self.before_toc[len(PREFIX):]
        self.path.write_bytes(self.with_toc(toc_bytes(shifted[::-1]), before))
        native = subprocess.run([FIXTURE_TOOL, "--check-codec", self.path], timeout=10, check=False)
        self.assertEqual(native.returncode, 0)
        keys = [b["key_escaped"] for b in self.inspect(0, "metadata_ok")["pmm_directory"]["blocks"]]
        self.assertEqual(keys, [r"opaque.\x85\x1b\x22\x5c\x0a", "extension.geometry"])

        first, second = self.rows
        starts_in_payload = self.offsets[0] + BLOCK_HEADER.size + len(b"extension.geometry") + 1
        self.path.write_bytes(self.with_toc(toc_bytes([first, overwrite(second, 16, "<Q",
                                                                       starts_in_payload)])))
        self.inspect(1, "overlapping_ranges", WRAPPER_BYTES + len(self.toc))

    def test_reader_budgets(self):
        def sample(count, key_size):
            before = bytearray(PREFIX)
            rows = []
            for i in range(count):
                size = key_size(i)
                key = f"{i:04d}".encode().ljust(size, b"x")
                rows.append(row(key, len(before)))
                before.extend(BLOCK_HEADER.pack(len(key), 1, 0, 0, 0, 0, 0) + key)
            toc = toc_bytes(rows)
            self.path.write_bytes(self.with_toc(toc, bytes(before)))
            return len(toc)

        self.assertLess(sample(2048, lambda _: 4), 256 * 1024)
        self.assertEqual(len(self.inspect(0, "metadata_ok")["pmm_directory"]["blocks"]), 2048)
        self.assertLess(sample(2049, lambda _: 4), 256 * 1024)
        self.inspect(3, "budget_exceeded", WRAPPER_BYTES + TOC_HEADER.size)
        self.assertEqual(sample(1024, lambda i: 216 if i < 1000 else 215), 256 * 1024)
        self.inspect(0, "metadata_ok")
        self.assertEqual(sample(1024, lambda i: 216 if i < 1001 else 215), 256 * 1024 + 1)
        self.inspect(3, "budget_exceeded", WRAPPER_BYTES)

    def test_zero_blocks_and_64bit_sparse_payload(self):
        empty_toc = toc_bytes([])
        self.path.write_bytes(self.with_toc(empty_toc, PREFIX))
        self.assertEqual(self.inspect(0, "metadata_ok", 118)["pmm_directory"]["blocks"], [])

        payload_size = (5 << 30) + 13
        key = b"sparse"
        toc = toc_bytes([row(key, len(PREFIX), payload_size)])
        toc_offset = len(PREFIX) + BLOCK_HEADER.size + len(key) + payload_size
        with self.path.open("wb") as stream:
            make_sparse(stream)
            stream.write(PREFIX + BLOCK_HEADER.pack(len(key), 1, 0, 0, payload_size, 0, 0) + key)
            stream.seek(toc_offset)
            stream.write(toc + footer_bytes(toc, toc_offset))
            stream.flush()
            os.fsync(stream.fileno())
        before = self.path.stat()
        allocation = allocated_bytes(self.path)
        self.assertLess(allocation, before.st_size)
        data = self.inspect(0, "metadata_ok", WRAPPER_BYTES + len(toc) + BLOCK_HEADER.size + len(key))
        self.assertEqual(data["pmm_directory"]["blocks"][0]["payload_bytes"], payload_size)
        after = self.path.stat()
        self.assertEqual((before.st_size, before.st_mtime_ns, allocation),
                         (after.st_size, after.st_mtime_ns, allocated_bytes(self.path)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
