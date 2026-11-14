# MAINTENANCE: exercise the ABI PowerShell packager with isolated fixture files, not a Windows build.
import hashlib
import pathlib
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    powershell, script_name = sys.argv[1:]
    script = pathlib.Path(script_name).resolve()
    source = script.parents[1]
    provider_path = pathlib.Path("legacy-mmdplugin/MMDPlugin.dll")
    provider_bytes = b"fixture provider\n"
    with tempfile.TemporaryDirectory(prefix="mms-abi-package-") as temporary:
        root = pathlib.Path(temporary)
        build = root / "build"
        binaries = build / "Release"
        provider = binaries / provider_path
        provider.parent.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source.as_posix()}\n"
            "MIKUMIKUSTUDIO_PROFILE:STRING=full\n"
            "CMAKE_GENERATOR_PLATFORM:STRING=x64\n",
            encoding="utf-8",
        )
        (binaries / "MikuMikuDance.exe").write_bytes(b"fixture host\n")
        (binaries / "unused.dll").write_bytes(b"should not ship\n")
        (provider.parent / "unused.pdb").write_bytes(b"should not ship\n")
        template = root / "ui-template.zip"
        template.write_bytes(b"fixture template\n")
        release = root / "release"
        release.mkdir()
        sentinel = release / "original.txt"
        sentinel.write_text("preserve on missing input", encoding="utf-8")
        command = [
            powershell, "-NoProfile", "-NonInteractive", "-File", str(script),
            "-BuildDirectory", str(build), "-ReleaseDirectory", str(release),
            "-UiTemplateArchive", str(template), "-Force",
        ]

        missing = subprocess.run(command, capture_output=True, text=True, errors="replace", check=False)
        check(missing.returncode != 0 and "MMDPlugin.dll" in missing.stderr,
              f"missing provider must fail explicitly: {missing.stderr}")
        check(sentinel.is_file(), "missing provider must fail before replacing the release directory")

        provider.write_bytes(provider_bytes)
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", check=False)
        check(result.returncode == 0, f"fixture packaging failed: {result.stderr}")
        check({item.name for item in release.iterdir()} == {
            "MikuMikuDance.exe", "legacy-mmdplugin", "licenses", "mms-adaptive-ui-template.zip",
        }, "unexpected top-level release contents")
        copied = release / provider_path
        check(copied.read_bytes() == provider_bytes, "provider copy has different bytes")
        check({item.name for item in copied.parent.iterdir()} == {copied.name},
              "provider directory contains other build files")
        expected_hash = hashlib.sha256(provider_bytes).hexdigest()
        check(f"{expected_hash}  {len(provider_bytes)}  {provider_path.as_posix()}" in result.stdout,
              "packager must report the provider checksum and relative path")
    print("PASS ABI release layout, provider hash and missing-input behavior")


if __name__ == "__main__":
    main()
