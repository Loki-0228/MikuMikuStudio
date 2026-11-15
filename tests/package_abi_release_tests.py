#!/usr/bin/env python3
"""Dedicated ABI staging contract. All EXE/DLL inputs are inert PE-header fixtures."""
from __future__ import annotations

import hashlib
import json
import importlib.util
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


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/package-abi-release.py"
PRODUCT = (
    "MikuMikuDance.exe", "MMDFacade.dll", "MMHack.dll", "legacy-mmdplugin/MMDPlugin.dll",
    "optional-plugins/first-person-camera/MMSFirstPersonCamera.dll",
)
FIXTURE_PAYLOAD = {
    "LICENSE", "PLUGINS/config.json", "licenses/Alembic-LICENSE.txt", "licenses/FXAA-LICENSE.txt",
    "licenses/Imath-LICENSE.md", "licenses/MinHook-LICENSE.txt", "licenses/README.md",
    "licenses/SMAA-LICENSE.txt",
}
CAMERA_DIR = "optional-plugins/first-person-camera"
EXTRA = {f"{CAMERA_DIR}/{name}": f"src/legacy_first_person/{name}" for name in ("LICENSE", "README.md")}
PACKAGE = set(PRODUCT) | FIXTURE_PAYLOAD | set(EXTRA) | {f"{CAMERA_DIR}/plugin.json", "mms-adaptive-ui-template.zip"}


def fake_pe(name: str, machine: int = 0x8664, is_dll: bool | None = None) -> bytes:
    header = bytearray(64 + 24 + 112)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 60, 64)
    if is_dll is None:
        is_dll = name.endswith(".dll")
    struct.pack_into("<4sHHIIIHHH", header, 64, b"PE\0\0", machine, 0, 0, 0, 0,
                     112, 0x2002 if is_dll else 0x0002, 0x020B)
    return bytes(header) + f"INERT TEST FIXTURE: {name}".encode()


class PackageAbiTest(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="mms-abi-contract-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "源码 space"
        self.build = self.root / "build tree"
        self.release = self.root / "output" / "new staging"
        self.template = self.root / "source template.zip"
        self.tool = self.root / "tools" / TOOL.name
        self.tool.parent.mkdir(parents=True)
        shutil.copyfile(TOOL, self.tool)
        for name in FIXTURE_PAYLOAD:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        for name in EXTRA.values():
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        for name in PRODUCT:
            target = self.build / "Release" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(fake_pe(name))
        self.write_cache()
        with zipfile.ZipFile(self.template, "w") as archive:
            archive.writestr("README.md", "source-only ABI packaging fixture")

    def write_cache(self, source: Path | None = None, profile: str = "full",
                    platform: str = "x64") -> None:
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source or self.root}\n"
            f"MIKUMIKUSTUDIO_PROFILE:STRING={profile}\n"
            f"CMAKE_GENERATOR_PLATFORM:STRING={platform}\n",
            encoding="utf-8",
        )

    def run_packager(self, *args: str, release: Path | None = None,
                     use_default_template: bool = False) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(self.tool), "--build-directory=build tree",
                   f"--release-directory={release or self.release}"]
        if not use_default_template:
            command += ["--ui-template-archive=source template.zip"]
        result = subprocess.run(command + list(args), capture_output=True, text=True,
                                cwd=ROOT, env={**os.environ, "PYTHONIOENCODING": "utf-8"},
                                encoding="utf-8")
        self.assertEqual(list(self.release.parent.glob(".mms-abi-stage-*")), [])
        return result

    def test_exact_files_and_hashes_with_build_and_repo_noise(self) -> None:
        for relative in ("build tree/Release/MMEffect.dll", "build tree/Release/MMSNvidiaAAFix.dll",
                         "build tree/Release/d3dx9_43.dll", "build tree/Release/MikuMikuDance.pdb",
                         "build tree/Release/legacy-mmdplugin/raw-observer.dll",
                         "PLUGINS/local-dev-plugin.dll", "licenses/local-notes.txt"):
            (self.root / relative).write_bytes(b"must not be copied")
        result = self.run_packager()
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = {path.relative_to(self.release).as_posix() for path in self.release.rglob("*")
                  if path.is_file()}
        self.assertEqual(actual, PACKAGE)
        lines = result.stdout.splitlines()
        for name, line in zip(sorted(PACKAGE), lines, strict=False):
            if name == f"{CAMERA_DIR}/plugin.json":
                expected = (self.release / name).read_bytes()
                manifest = json.loads(expected)
                self.assertEqual(manifest['profile'], 'first_person_camera_v4')
                self.assertEqual(manifest['sha256'], hashlib.sha256((self.release / CAMERA_DIR / manifest['entry']).read_bytes()).hexdigest())
            elif name in EXTRA:
                expected = (self.root / EXTRA[name]).read_bytes()
            else:
                expected = ((self.build / "Release" / name) if name in PRODUCT
                            else self.root / name if name in FIXTURE_PAYLOAD else self.template).read_bytes()
            self.assertEqual((self.release / name).read_bytes(), expected)
            self.assertEqual(line, f"{hashlib.sha256(expected).hexdigest()}  {len(expected)}  {name}")
        self.assertEqual(len(lines), len(PACKAGE) + 1)

    def test_missing_required_build_and_repository_files(self) -> None:
        for root, name in [(self.build / "Release", n) for n in PRODUCT] + [
            (self.root, "PLUGINS/config.json"), (self.root, "LICENSE"), (self.root, "licenses/README.md")
        ]:
            with self.subTest(name=name):
                path = root / name
                initial = path.read_bytes()
                path.unlink()
                result = self.run_packager()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(path.name, result.stderr)
                self.assertFalse(self.release.exists())
                path.write_bytes(initial)

    def test_cache_must_describe_this_full_x64_source(self) -> None:
        for fields in ({"source": self.root.parent}, {"profile": "mmd932"}, {"platform": "Win32"}):
            with self.subTest(fields=fields):
                self.write_cache(**fields)
                self.assertNotEqual(self.run_packager().returncode, 0)
                self.assertFalse(self.release.exists())

    def test_requires_x64_executable_and_dll_headers(self) -> None:
        name = "MMHack.dll"
        for source in (fake_pe(name, machine=0x14C), fake_pe(name, is_dll=False), b"MZ"):
            with self.subTest(source=source):
                (self.build / "Release" / name).write_bytes(source)
                result = self.run_packager()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stderr)
                self.assertFalse(self.release.exists())

    def test_custom_template_must_exist_and_be_a_zip(self) -> None:
        self.template.unlink()
        self.assertNotEqual(self.run_packager().returncode, 0)
        self.assertFalse(self.release.exists())
        self.template.write_text("not a zip", encoding="utf-8")
        self.assertNotEqual(self.run_packager().returncode, 0)
        self.assertFalse(self.release.exists())
        self.assertFalse((self.root / "build/packages").exists())

    def test_default_template_generation_and_refresh(self) -> None:
        default = self.root / "build/packages/mms-adaptive-ui-template.zip"
        (self.tool.parent / "package-adaptive-ui-template.py").write_text(
            "from pathlib import Path\n"
            "import sys, zipfile\n"
            "output = Path(__file__).resolve().parents[1] / 'build/packages/mms-adaptive-ui-template'\n"
            "assert ('--refresh' in sys.argv) == output.exists()\n"
            "output.mkdir(parents=True, exist_ok=True)\n"
            "with zipfile.ZipFile(output.with_suffix('.zip'), 'w') as archive:\n"
            "    archive.writestr('README.md', 'default generator fixture')\n",
            encoding="utf-8",
        )
        result = self.run_packager(use_default_template=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.release / default.name).read_bytes(), default.read_bytes())
        default.unlink()
        result = self.run_packager("--force", use_default_template=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.release / default.name).read_bytes(), default.read_bytes())

    def test_nonempty_does_not_replace_without_force_or_if_preflight_fails(self) -> None:
        self.release.mkdir(parents=True)
        old = self.release / "MikuMikuDance.exe"
        old.write_bytes(b"previous package placeholder")
        self.assertNotEqual(self.run_packager().returncode, 0)
        (self.build / "Release/MMHack.dll").unlink()
        self.assertNotEqual(self.run_packager("--force").returncode, 0)
        self.assertEqual(old.read_bytes(), b"previous package placeholder")

    def test_force_replaces_only_known_staging_contents(self) -> None:
        self.release.mkdir(parents=True)
        old = self.release / "MikuMikuDance.exe"
        extra = self.release / "my-project.pmm"
        old.write_bytes(b"previous package placeholder")
        extra.write_bytes(b"preserve")
        self.assertNotEqual(self.run_packager("--force").returncode, 0)
        self.assertEqual(extra.read_bytes(), b"preserve")
        self.assertEqual(old.read_bytes(), b"previous package placeholder")
        extra.unlink()
        result = self.run_packager("--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(old.read_bytes(), fake_pe(old.name))

    def test_force_refuses_input_directory(self) -> None:
        for release in (self.root, self.build, self.build / "Release"):
            with self.subTest(release=release):
                self.assertNotEqual(self.run_packager("--force", release=release).returncode, 0)
                self.assertTrue((self.build / "Release/MMHack.dll").is_file())

    def load_core(self):
        spec = importlib.util.spec_from_file_location("abi_release_fixture", TOOL)
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        return tool

    def prepare_previous_release(self) -> Path:
        self.release.mkdir(parents=True)
        old = self.release / "MikuMikuDance.exe"
        old.write_bytes(b"previous package placeholder")
        return old

    def test_failed_copy_leaves_previous_release_and_cleans_stage(self) -> None:
        tool = self.load_core()
        old = self.prepare_previous_release()
        real_copy = shutil.copyfile

        def fail_after_first_file(source: Path, destination: Path) -> None:
            if destination.name == "MMHack.dll":
                raise OSError("injected copy failure")
            real_copy(source, destination)

        with patch.object(tool.shutil, "copyfile", side_effect=fail_after_first_file):
            with self.assertRaisesRegex(OSError, "injected copy failure"):
                tool.stage_package(self.root, self.build, self.release, self.template, True)
        self.assertEqual(old.read_bytes(), b"previous package placeholder")
        self.assertEqual(list(self.release.parent.glob(".mms-abi-stage-*")), [])


    def test_failed_stage_rename_restores_previous_release(self) -> None:
        tool = self.load_core()
        old = self.prepare_previous_release()
        real_rename = Path.rename

        def fail_stage_rename(source: Path, destination: Path) -> Path:
            if source.name == "stage":
                raise OSError("injected stage rename failure")
            return real_rename(source, destination)

        with patch.object(Path, "rename", fail_stage_rename):
            with self.assertRaisesRegex(OSError, "injected stage rename failure"):
                tool.stage_package(self.root, self.build, self.release, self.template, True)
        self.assertEqual(old.read_bytes(), b"previous package placeholder")
        self.assertEqual(list(self.release.parent.glob(".mms-abi-stage-*")), [])

    def test_failed_rollback_keeps_previous_release_recoverable(self) -> None:
        tool = self.load_core()
        old = self.prepare_previous_release()
        original = old.read_bytes()
        real_rename = Path.rename

        def fail_stage_and_rollback(source: Path, destination: Path) -> Path:
            if source.name in {"stage", "previous"}:
                raise OSError("injected rename failure")
            return real_rename(source, destination)

        with patch.object(Path, "rename", fail_stage_and_rollback):
            with self.assertRaisesRegex(RuntimeError, "previous"):
                tool.stage_package(self.root, self.build, self.release, self.template, True)
        self.assertFalse(self.release.exists())
        backup, = self.release.parent.glob(".mms-abi-stage-*/previous/MikuMikuDance.exe")
        self.assertEqual(backup.read_bytes(), original)

    def test_linked_provider_is_not_copied(self) -> None:
        runtime = self.build / "Release"
        provider = runtime / "legacy-mmdplugin"
        outside = self.root / "other provider"
        provider.rename(outside)
        try:
            provider.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"directory symlinks unavailable: {error}")
        result = self.run_packager()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(provider.name, result.stderr)
        self.assertFalse(self.release.exists())


if __name__ == "__main__":
    unittest.main()
