#!/usr/bin/env python3
"""Stage the full Windows x64 ABI package from an existing Release build (Python 3.10+)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile


BUILD_FILES = (
    "MikuMikuDance.exe",
    "MMDFacade.dll",
    "MMHack.dll",
    "PLUGINS/LEGACY/MMDPlugin.dll",
    "PLUGINS/first-person-camera/MMSFirstPersonCamera.dll",
)
CAMERA_DIR = "PLUGINS/first-person-camera"
EXTRA_FILES = {f"{CAMERA_DIR}/{name}": f"src/legacy_first_person/{name}" for name in ("LICENSE", "README.md")}
# Do not copy entire directories from a development checkout.
REPO_FILES = (
    "LICENSE",
    "PLUGINS/config.json",
    "licenses/Alembic-LICENSE.txt",
    "licenses/FXAA-LICENSE.txt",
    "licenses/Imath-LICENSE.md",
    "licenses/MinHook-LICENSE.txt",
    "licenses/README.md",
    "licenses/SMAA-LICENSE.txt",
)
DEFAULT_BUILD = Path("build/cmake")
DEFAULT_RELEASE = Path("build/release")
DEFAULT_TEMPLATE = Path("build/packages/mms-adaptive-ui-template.zip")
PACKAGE_FILES = frozenset((*BUILD_FILES, *REPO_FILES, *EXTRA_FILES, f"{CAMERA_DIR}/plugin.json", DEFAULT_TEMPLATE.name))
PACKAGE_DIRS = frozenset(("PLUGINS/LEGACY", "PLUGINS", "licenses", CAMERA_DIR))
# Recognize only previous package-owned providers and camera package when rebuilding staging.
# New output never contains these old locations, and adjacent user files still fail.
PREVIOUS_PROVIDER_DIRS = ("PLUGINS/legacy-mmdplugin", "legacy-mmdplugin")
PREVIOUS_CAMERA_DIR = "optional-plugins/first-person-camera"
PREVIOUS_PACKAGE_DIRS = frozenset((*PREVIOUS_PROVIDER_DIRS, "optional-plugins", PREVIOUS_CAMERA_DIR))
PREVIOUS_PACKAGE_FILES = frozenset(
    [f"{directory}/MMDPlugin.dll" for directory in PREVIOUS_PROVIDER_DIRS] +
    [f"{PREVIOUS_CAMERA_DIR}/{name}" for name in ("MMSFirstPersonCamera.dll", "plugin.json", "LICENSE", "README.md")])


def cache_value(cache: str, name: str) -> str:
    match = re.search(rf"^{name}:[^=\n]+=(.*)$", cache, re.MULTILINE)
    return match[1].strip() if match else ""


def is_link(path: Path) -> bool:
    try:
        mode = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(mode.st_mode) or bool(
        getattr(mode, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    )


def require_file(path: Path, root: Path | None = None) -> None:
    if not path.is_file() or is_link(path):
        raise ValueError(f"缺少普通文件：{path}")
    if root is not None:
        for directory in path.parents:
            if directory == root:
                break
            if is_link(directory):
                raise ValueError(f"打包输入目录不能是链接：{directory}")


def require_x64_pe(path: Path) -> None:
    """Catch wrong architecture and wrongly named inputs; not a dependency audit."""
    with path.open("rb") as binary:
        dos = binary.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise ValueError(f"不是 Windows x64 PE 文件：{path}")
        (offset,) = struct.unpack_from("<I", dos, 60)
        size = binary.seek(0, 2)
        if not 64 <= offset <= size - 26:
            raise ValueError(f"无效的 PE 文件头：{path}")
        binary.seek(offset)
        header = binary.read(26)
        signature, machine, _, _, _, _, optional_size, flags, magic = struct.unpack(
            "<4sHHIIIHHH", header
        )
        dll_flag = 0x2000 if path.suffix == ".dll" else 0
        if (signature != b"PE\0\0" or machine != 0x8664 or magic != 0x20B
                or optional_size < 112 or offset + 24 + optional_size > size
                or (flags & 0x2002) != (dll_flag | 0x0002)):
            raise ValueError(f"不是预期的 Windows x64 PE 文件：{path}")


def check_release(release: Path, force: bool) -> None:
    if is_link(release) or (release.exists() and not release.is_dir()):
        raise ValueError(f"发布路径必须是普通目录：{release}")
    if not release.exists() or not any(release.iterdir()):
        return
    if not force:
        raise ValueError(f"发布目录非空，确认要重建时加 -Force：{release}")
    for path in release.rglob("*"):
        name = path.relative_to(release).as_posix()
        if (is_link(path)
                or not ((path.is_dir() and name in PACKAGE_DIRS | PREVIOUS_PACKAGE_DIRS)
                        or (path.is_file() and name in PACKAGE_FILES | PREVIOUS_PACKAGE_FILES))):
            raise ValueError(f"发布目录含非打包文件，拒绝覆盖：{path}")


def require_template(repo: Path, template: Path) -> None:
    if not template.exists() and template == (repo / DEFAULT_TEMPLATE).resolve():
        command = [sys.executable, str(repo / "tools/package-adaptive-ui-template.py")]
        if template.with_suffix("").exists():
            command.append("--refresh")
        subprocess.run(command, check=True)
    require_file(template)
    if not zipfile.is_zipfile(template):
        raise ValueError(f"GUI 模块源码包不是 ZIP：{template}")


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage_package(repo: Path, build: Path, release: Path, template: Path, force: bool) -> None:
    build = (repo / build).resolve()
    release = repo / release
    if is_link(release):
        raise ValueError(f"发布路径必须是普通目录：{release}")
    release = release.resolve()
    template = (repo / template).resolve()
    if (repo.is_relative_to(release) or build.is_relative_to(release)
            or release.is_relative_to(build) or template.is_relative_to(release)):
        raise ValueError(f"发布目录不能与源码根、构建目录或模板文件重叠：{release}")
    check_release(release, force)

    cache_path = build / "CMakeCache.txt"
    require_file(cache_path)
    cache = cache_path.read_text(encoding="utf-8")
    source = cache_value(cache, "CMAKE_HOME_DIRECTORY")
    profile = cache_value(cache, "MIKUMIKUSTUDIO_PROFILE")
    platform = cache_value(cache, "CMAKE_GENERATOR_PLATFORM")
    if not Path(source).is_absolute() or Path(source).resolve() != repo:
        raise ValueError(f"构建目录不属于当前源码：{build}")
    if profile != "full":
        raise ValueError(f"发布目录要求完整档构建，当前 MIKUMIKUSTUDIO_PROFILE 为 '{profile}'：{build}")
    if platform != "x64":
        raise ValueError(f"发布目录要求 x64 构建：{build}")

    files = {name: build / "Release" / name for name in BUILD_FILES}
    for path in files.values():
        require_file(path, build)
        require_x64_pe(path)
    for name in REPO_FILES:
        path = repo / name
        require_file(path, repo)
        files[name] = path
    for name, source in EXTRA_FILES.items():
        path = repo / source
        require_file(path, repo)
        files[name] = path
    require_template(repo, template)
    files[DEFAULT_TEMPLATE.name] = template

    release.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".mms-abi-stage-", dir=release.parent))
    stage, previous = work / "stage", work / "previous"
    published = False
    try:
        report = []
        for name, source_path in sorted(files.items()):
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_path, target)
            report.append(f"{file_digest(target)}  {target.stat().st_size}  {name}")

        name = f"{CAMERA_DIR}/plugin.json"
        target = stage / name
        target.write_text(json.dumps(dict(schemaVersion=1, id="org.mikumikustudio.legacy.first-person-camera",
            name="FirstPersonCamera (MMS)", version="1.0.0", kind="legacy_mmdplugin",
            profile="first_person_camera_v4", entry="MMSFirstPersonCamera.dll",
            sha256=file_digest(stage / CAMERA_DIR / "MMSFirstPersonCamera.dll"),
            architecture="x64", minWindows="6.1.0", reloadPolicy="restart"), indent=2) + "\n", encoding="utf-8")
        report.append(f"{file_digest(target)}  {target.stat().st_size}  {name}")
        report.sort(key=lambda line: line.split("  ", 2)[2])

        check_release(release, force)
        try:
            if release.exists():
                release.rename(previous)
            stage.rename(release)
            published = True
        except (OSError, KeyboardInterrupt):
            if previous.exists() and not release.exists():
                try:
                    previous.rename(release)
                except OSError as error:
                    raise RuntimeError(f"无法恢复先前的发布目录；已保留在：{previous}") from error
            raise
    finally:
        if published or not previous.exists():
            try:
                shutil.rmtree(work)
            except OSError as error:
                print(f"无法清理打包临时目录 {work}：{error}", file=sys.stderr)
    print("\n".join(report))
    print(f"发布目录已生成：{release}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--release-directory", type=Path, default=DEFAULT_RELEASE)
    parser.add_argument("--ui-template-archive", type=Path, default=DEFAULT_TEMPLATE)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    try:
        stage_package(repo, args.build_directory, args.release_directory, args.ui_template_archive, args.force)
    except (ValueError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
