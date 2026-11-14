"""Independently decode one still PNG or PNG-in-MOV and check expected alpha samples."""

import argparse
from fractions import Fraction
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import warnings

from run_windows_abi_candidate import digest, require, require_file


MAX_DECODE_BYTES = 128 * 1024 * 1024


def still_rgba(path, width, height):
    from PIL import Image

    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(path) as image:
            require(image.format == "PNG" and image.size == (width, height) and image.n_frames == 1,
                    f"Expected a single {width}×{height} PNG: {path}")
            image.verify()
        with Image.open(path) as image:
            image.load()
            return image.convert("RGBA").tobytes()


def media_command(program, options, path):
    command = shutil.which(program)
    require(command, f"{program} is not available on PATH")
    result = subprocess.run([command, *options], cwd=path.parent, timeout=120, capture_output=True)
    if result.returncode or result.stderr:
        raise ValueError(f"{program} exit {result.returncode}: {result.stderr.decode('utf-8', 'replace').strip()}")
    return result.stdout


def mov_rgba(path, width, height, frames, fps):
    with path.open("rb") as stream:
        require(stream.read(12)[4:] == b"ftypqt  ", "Expected a QuickTime-branded MOV")
    common = ["-v", "error", "-protocol_whitelist", "file", "-f", "mov", "-enable_drefs", "0"]
    metadata = json.loads(media_command("ffprobe", [*common, "-show_streams", "-of", "json", "-i", str(path)], path))
    streams = metadata["streams"]
    require(len(streams) == 1 and streams[0].get("codec_type") == "video" and streams[0].get("codec_name") == "png",
            "Expected one PNG video stream in the MOV, with no other streams")
    stream = streams[0]
    require((stream.get("width"), stream.get("height")) == (width, height),
            f"Expected MOV dimensions {width}×{height}; got {stream.get('width')}×{stream.get('height')}")
    require(Fraction(stream.get("avg_frame_rate", "0")) == fps,
            f"Expected MOV average frame rate {fps}; got {stream.get('avg_frame_rate')}")
    pixels = media_command("ffmpeg", [*common, "-nostdin", "-xerror", "-noautorotate", "-i", str(path),
                                     "-map", "0:v:0", "-vsync", "0", "-frames:v", str(frames + 1),
                                     "-pix_fmt", "rgba", "-f", "rawvideo", "-"], path)
    frame_size = width * height * 4
    require(len(pixels) == frames * frame_size,
            f"Expected {frames} fully decoded frames; got {len(pixels) // frame_size} with {len(pixels) % frame_size} trailing bytes")
    return pixels


def verify(path, width, height, samples, *, frames=None, fps=None, exe=None, exe_hash=None):
    path = require_file(path.absolute())
    require(not path.parent.is_symlink() and set(path.parent.iterdir()) == {path},
            f"Expected only the export in a dedicated directory: {path.parent}")
    kind = path.suffix.lower()
    require(kind in {".png", ".mov"}, "Export must end in .png or .mov")
    require(width > 0 and height > 0, "Dimensions must be positive")
    require({(0, 0), (255, 255)}.issubset({(lo, hi) for _, _, lo, hi in samples}),
            "Provide at least one known clear pixel (0,0) and one known opaque pixel (255,255)")
    require(len({(x, y) for x, y, _, _ in samples}) == len(samples), "Sample coordinates must be distinct")
    for x, y, lo, hi in samples:
        require(0 <= x < width and 0 <= y < height and 0 <= lo <= hi <= 255, f"Invalid sample: {x} {y} {lo} {hi}")
    require(bool(exe) == bool(exe_hash), "--candidate-exe and --candidate-sha256 must be used together")
    if exe:
        require(re.fullmatch(r"[0-9a-fA-F]{64}", exe_hash), "Expected a 64-digit SHA-256")
        require(digest(require_file(exe)) == exe_hash.lower(), "Candidate executable SHA-256 mismatch")
    export_hash = digest(path)
    if kind == ".mov":
        require(frames and frames > 0 and fps and fps > 0, "MOV requires positive --frames and --fps")
        require(width * height * 4 * (frames + 1) <= MAX_DECODE_BYTES, "Use a short MOV below 128 MiB decoded")
        pixels = mov_rgba(path, width, height, frames, fps)
    else:
        require(frames is None and fps is None, "--frames and --fps apply only to MOV")
        require(width * height * 4 <= MAX_DECODE_BYTES, "Still PNG exceeds 128 MiB decoded")
        frames = 1
        pixels = still_rgba(path, width, height)
    measured = []
    for x, y, lo, hi in samples:
        observed = []
        for frame in range(frames):
            value = pixels[((frame * height + y) * width + x) * 4 + 3]
            require(lo <= value <= hi, f"Frame {frame + 1} pixel ({x},{y}) alpha={value}; expected [{lo},{hi}]")
            observed.append(value)
        measured.append({"xy": [x, y], "expected_range": [lo, hi],
                         "observed_range": [min(observed), max(observed)]})
    require(digest(path) == export_hash, "Export changed while it was being checked")
    return {"decoded_alpha_samples": "PASS", "file": str(path), "sha256": export_hash,
            "dimensions": [width, height], "decoded_frames": frames, "fps": str(fps) if fps else None,
            "alpha_samples_each_frame": measured, "candidate_exe_sha256": exe_hash.lower() if exe_hash else None,
            "not_verified": ["origin of export", "product menu or chooser", "active project or window state", "timeline order"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path, help="only file in a dedicated output directory")
    parser.add_argument("--size", type=int, metavar=("WIDTH", "HEIGHT"), nargs=2, required=True)
    parser.add_argument("--sample", type=int, metavar=("X", "Y", "MIN_ALPHA", "MAX_ALPHA"), nargs=4,
                        action="append", required=True, help="top-left-based coordinate; repeated for each sample")
    parser.add_argument("--frames", type=int, help="expected MOV frame count")
    parser.add_argument("--fps", help="expected MOV rate, e.g. 30 or 30000/1001")
    parser.add_argument("--candidate-exe", type=Path)
    parser.add_argument("--candidate-sha256")
    args = parser.parse_args(argv)
    try:
        result = verify(args.file, *args.size, args.sample, frames=args.frames,
                        fps=Fraction(args.fps) if args.fps else None, exe=args.candidate_exe,
                        exe_hash=args.candidate_sha256)
    except (ImportError, KeyError, OSError, RuntimeError, SyntaxError, ValueError, Warning,
            ZeroDivisionError, subprocess.SubprocessError) as error:
        print(f"FAIL or unavailable: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
