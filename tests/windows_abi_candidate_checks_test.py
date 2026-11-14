"""Host-only self-test: validate evidence checks without invoking Windows."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from run_windows_abi_candidate import (
    CTEST_CASES, HOST, MANUAL_CASES, PROVIDER, TEMPLATE, Evidence,
    cache_values, digest, require_pe_x64, verify_package, verify_template,
)
from verify_candidate_export_alpha import verify


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mms-candidate-check-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def put(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path


class PackageTests(Fixture):
    def setUp(self):
        super().setUp()
        self.release, self.install, self.build, self.licenses = (
            self.root / name for name in ("release", "install", "build", "licenses"))
        self.archive = self.put(self.root / "template.zip", b"copy check")
        self.config = self.put(self.root / "config.json", b"{}")
        self.put(self.licenses / "notice.txt", b"license")
        for tree in (self.release, self.install, self.build):
            self.put(tree / HOST, b"host")
            self.put(tree / PROVIDER, b"provider")
        for tree in (self.release, self.install):
            self.put(tree / "licenses/notice.txt", b"license")
        self.put(self.release / TEMPLATE, self.archive.read_bytes())
        self.put(self.install / "PLUGINS/config.json", self.config.read_bytes())
        self.put(self.install / "an-allowed-runtime.dll", b"runtime")

    def check(self):
        return verify_package(self.release, self.install, self.build, self.archive, self.licenses, self.config)

    def test_exact_release_and_install_provider(self):
        self.assertEqual(self.check()[PROVIDER.as_posix()]["sha256"], digest(self.build / PROVIDER))

    def test_no_provider_under_plugins_or_root(self):
        for duplicate in (self.install / "PLUGINS/MMDPlugin.dll", self.install / "MMDPlugin.dll"):
            with self.subTest(duplicate=duplicate):
                self.put(duplicate, b"wrong")
                with self.assertRaisesRegex(ValueError, "single private"):
                    self.check()
                duplicate.unlink()

    def test_refuse_release_extra_or_changed_bytes(self):
        extra = self.put(self.release / "unrelated.log", b"stray")
        with self.assertRaisesRegex(ValueError, "top-level release"):
            self.check()
        extra.unlink()
        self.put(self.release / PROVIDER, b"changed")
        with self.assertRaisesRegex(ValueError, "SHA-256 differs"):
            self.check()

    def test_refuse_license_omission(self):
        (self.install / "licenses/notice.txt").unlink()
        with self.assertRaisesRegex(ValueError, "license inventory"):
            self.check()

    def test_cache_and_x64_header_preflight(self):
        fixture = bytearray(90)
        fixture[:2] = b"MZ"
        struct.pack_into("<I", fixture, 60, 64)
        fixture[64:70], fixture[88:90] = b"PE\0\0\x64\x86", b"\x0b\x02"
        executable = self.put(self.build / HOST, fixture)
        self.assertEqual(require_pe_x64(executable), executable)
        fixture[68:70] = b"\x4c\x01"
        self.put(executable, fixture)
        with self.assertRaisesRegex(ValueError, r"PE32\+ x64"):
            require_pe_x64(executable)
        cache = self.build / "CMakeCache.txt"
        base = (f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root.as_posix()}\n"
                "CMAKE_GENERATOR_PLATFORM:STRING=x64\n"
                "MIKUMIKUSTUDIO_PROFILE:STRING=full\n"
                "MIKUMIKUSTUDIO_BUILD_UI_TESTS:BOOL=")
        self.put(cache, (base + "ON\n").encode())
        self.assertEqual(cache_values(self.build, self.root)["MIKUMIKUSTUDIO_PROFILE"], "full")
        with self.assertRaisesRegex(ValueError, "different checkout"):
            cache_values(self.build, self.install)
        self.put(cache, (base + "OFF\n").encode())
        with self.assertRaisesRegex(ValueError, "BUILD_UI_TESTS"):
            cache_values(self.build, self.root)

    def test_template_identity_and_members(self):
        files = {"LICENSE": b"license", "CMakeLists.txt": b"cmake", "README.md": b"readme",
                 "third_party/native_ui/CMakeLists.txt": b"ui"}
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        source = {"head": "current", "files_sha256": {"LICENSE": hashes["LICENSE"],
                  "third_party/native_ui/CMakeLists.txt": hashes["third_party/native_ui/CMakeLists.txt"],
                  "tools/package-adaptive-ui-template.py": "packager"}}
        manifest = {"schemaVersion": 1, "name": "mms-adaptive-ui-template", "binariesIncluded": False,
                    "sourceCommit": "current", "packagerSha256": "packager", "files": hashes}

        def write_zip():
            with zipfile.ZipFile(self.archive, "w") as archive:
                archive.writestr("template/MANIFEST.json", json.dumps(manifest))
                for name, data in files.items():
                    archive.writestr(f"template/{name}", data)

        write_zip()
        verify_template(self.archive, source)
        manifest["sourceCommit"] = "old"
        write_zip()
        with self.assertRaisesRegex(ValueError, "identify this checkout"):
            verify_template(self.archive, source)
        manifest["sourceCommit"] = "current"
        files["README.md"] = b"tampered"
        write_zip()
        with self.assertRaisesRegex(ValueError, "member SHA-256 differs"):
            verify_template(self.archive, source)

    @unittest.skipIf(os.name == "nt", "non-Windows safety check")
    def test_windows_runner_does_not_create_output_on_linux(self):
        evidence = self.root / "new"
        result = subprocess.run([sys.executable, "-B", str(Path(__file__).with_name("run_windows_abi_candidate.py")),
                                 "--build", str(self.build), "--ui-template", str(self.archive),
                                 "--evidence", str(evidence)], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"Windows only", result.stderr)
        self.assertFalse(evidence.exists())

    def test_empty_ctest_and_missing_marker_never_pass(self):
        evidence = Evidence(self.root)

        def fake_run(*args, **kwargs):
            kwargs["stdout"].write(b"No tests were found!!!\n")
            return subprocess.CompletedProcess(args, returncode=0)

        with patch("run_windows_abi_candidate.subprocess.CREATE_NO_WINDOW", 0, create=True), \
                patch("run_windows_abi_candidate.subprocess.run", side_effect=fake_run):
            self.assertFalse(evidence.command(CTEST_CASES[0], ["ctest"], self.root, 1, no_tests=True))
            self.assertFalse(evidence.command("probe", ["image_sequence_tests.exe"], self.root, 1, marker="PASS"))
        summary = json.loads((self.root / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["automated"][CTEST_CASES[0]]["status"], "BLOCKED")
        self.assertEqual(summary["automated"]["probe"]["status"], "FAIL")
        self.assertEqual(summary["manual"], dict.fromkeys(MANUAL_CASES, "NOT RUN"))


class AlphaTests(Fixture):
    size = (8, 8)
    samples = [(1, 1, 0, 0), (6, 5, 255, 255), (5, 2, 126, 130)]

    def setUp(self):
        super().setUp()
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow unavailable")
        self.image = Image.new("RGBA", self.size, (0, 0, 255, 255))
        self.image.putpixel((1, 1), (0, 0, 0, 0))
        self.image.putpixel((5, 2), (255, 0, 0, 128))
        directory = self.root / "png"
        directory.mkdir()
        self.still = directory / "still.png"
        self.image.save(self.still)

    def test_still_alpha_and_hash(self):
        result = verify(self.still, *self.size, self.samples)
        self.assertEqual(result["decoded_frames"], 1)
        self.assertEqual(result["sha256"], digest(self.still))
        self.assertEqual(result["alpha_samples_each_frame"][2]["observed_range"], [128, 128])
        self.assertIn("product menu or chooser", result["not_verified"])

    def test_wrong_alpha_or_dimensions(self):
        with self.assertRaisesRegex(ValueError, "Expected a single"):
            verify(self.still, 9, 8, self.samples)
        self.image.putpixel((1, 1), (0, 0, 0, 255))
        self.image.save(self.still)
        with self.assertRaisesRegex(ValueError, "alpha=255"):
            verify(self.still, *self.size, self.samples)

    def test_truncation_and_leftovers(self):
        self.put(self.still.parent / "still.tmp", b"leftover")
        with self.assertRaisesRegex(ValueError, "dedicated directory"):
            verify(self.still, *self.size, self.samples)
        (self.still.parent / "still.tmp").unlink()
        self.still.write_bytes(self.still.read_bytes()[:36])
        with self.assertRaises((SyntaxError, OSError)):
            verify(self.still, *self.size, self.samples)

    def test_candidate_hash_binding(self):
        candidate = self.put(self.root / "candidate.exe", b"candidate")
        self.assertEqual(verify(self.still, *self.size, self.samples, exe=candidate,
                                exe_hash=digest(candidate))["candidate_exe_sha256"], digest(candidate))
        with self.assertRaisesRegex(ValueError, "executable SHA-256 mismatch"):
            verify(self.still, *self.size, self.samples, exe=candidate, exe_hash="0" * 64)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg tools unavailable")
    def test_mov_codec_frame_count_rate_and_late_alpha_failure(self):
        from fractions import Fraction

        directory = self.root / "mov"
        directory.mkdir()
        frames = self.root / "frames"
        frames.mkdir()
        for n in range(3):
            self.image.save(frames / f"frame{n}.png")
        mov = directory / "out.mov"

        def encode(codec, pixel_format):
            subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-y", "-framerate", "30", "-i",
                            str(frames / "frame%d.png"), "-c:v", codec, "-pix_fmt", pixel_format, str(mov)],
                           timeout=30, check=True, capture_output=True)

        def check(count=3, fps=30):
            return verify(mov, *self.size, self.samples, frames=count, fps=Fraction(fps))

        encode("png", "rgba")
        self.assertEqual(check()["decoded_frames"], 3)
        with self.assertRaisesRegex(ValueError, "fully decoded frames"):
            check(count=2)
        with self.assertRaisesRegex(ValueError, "frame rate"):
            check(fps=24)
        encode("mpeg4", "yuv420p")
        with self.assertRaisesRegex(ValueError, "one PNG video stream"):
            check()
        self.image.putpixel((1, 1), (0, 0, 0, 255))
        self.image.save(frames / "frame2.png")
        encode("png", "rgba")
        with self.assertRaisesRegex(ValueError, r"Frame 3 pixel .* alpha=255"):
            check()


if __name__ == "__main__":
    unittest.main(verbosity=2)
