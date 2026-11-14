#!/usr/bin/env python3
"""Verify the historical Bullet source ZIP and optionally stage a new Conan recipe."""

import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path, PurePosixPath
import shutil
import zipfile


ARCHIVE_SHA256 = "fe2e369593b034c00a1245676edd8417a58c8030a94e061220f0c228ca61fe8f"
PREFIX = "bullet-2.75/"
REFERENCE = "bullet275/2.75#"
ROOT = Path(__file__).resolve().parents[1]


def verify(archive, recipe, locked_revision):
    if hashlib.sha256(archive.getbuffer()).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Bullet ZIP SHA-256 does not match the official archived release")
    hashes = {"conanfile.py": hashlib.md5(recipe, usedforsecurity=False).hexdigest()}
    files = {}
    names = set()
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if item.is_dir():
                continue
            name = item.filename.removeprefix(PREFIX)
            parts = name.split("/")
            if (not item.filename.startswith(PREFIX) or any(part in ("", ".", "..") for part in parts)
                    or "\\" in name or ":" in name or name.casefold() in names):
                raise ValueError(f"Invalid or duplicate ZIP member: {item.filename}")
            names.add(name.casefold())
            content = source.read(item)
            files[PurePosixPath(name)] = content
            hashes[f"export_source/bullet-src/{name}"] = hashlib.md5(content, usedforsecurity=False).hexdigest()

    # Conan's default hash revision is the MD5 of its sorted file manifest.
    manifest = "".join(f"{name}: {value}\n" for name, value in sorted(hashes.items()))
    actual = hashlib.md5(manifest.encode("utf-8"), usedforsecurity=False).hexdigest()
    if actual != locked_revision:
        raise ValueError(f"Conan recipe revision mismatch: {actual} != {locked_revision}; check recipe bytes (LF)")
    return files


def stage(directory, recipe, files):
    directory.mkdir()
    try:
        (directory / "conanfile.py").write_bytes(recipe)
        for name, content in files.items():
            output = directory / "bullet-src" / Path(*name.parts)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(content)
    except OSError:
        shutil.rmtree(directory)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="local copy of the official bullet-2.75.zip")
    parser.add_argument("--stage-recipe", type=Path, help="new directory outside the source checkout")
    args = parser.parse_args()
    try:
        lock = json.loads((ROOT / "profiles/win7-x64.lock").read_bytes())
        revisions = [ref.removeprefix(REFERENCE).split("%", 1)[0]
                     for ref in lock["requires"] if ref.startswith(REFERENCE)]
        if len(revisions) != 1:
            raise ValueError("Expected one locked Bullet 2.75 recipe revision")
        recipe = (ROOT / "recipes/bullet275/conanfile.py").read_bytes()
        files = verify(BytesIO(args.archive.read_bytes()), recipe, revisions[0])
        if args.stage_recipe is not None:
            destination = args.stage_recipe.resolve()
            if destination.is_relative_to(ROOT):
                raise ValueError("Stage the recipe outside the source checkout")
            stage(destination, recipe, files)
        print(f"Verified {len(files)} Bullet source archive files, {REFERENCE}{revisions[0]}")
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
