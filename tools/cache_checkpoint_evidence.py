#!/usr/bin/env python3
"""Inspect one or more preserved checkpoint blobs and run the Linux process experiment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import stat
import struct
import subprocess
import sys
import tempfile

HEADER = 128
PAYLOAD_MAX = 376 + 192 * 160
OWNER = b"MMSCatalogHead01"
KEY = bytes([3]) + bytes(31)
NAME = re.compile(r"[0-9a-f]{32}\.blob\Z")
TIMEOUT = 10


def checked(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).digest()


def file_identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def read_evidence(path):
    path = Path(os.path.abspath(path))
    for parent in reversed(path.parents):
        checked(stat.S_ISDIR(parent.lstat().st_mode), "evidence parent is not an ordinary directory")
    before = path.lstat()
    checked(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, "evidence must be an ordinary, non-hardlinked file")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        opened = os.fstat(stream.fileno())
        checked(file_identity(opened) == file_identity(before), "evidence changed during open")
        data = stream.read(HEADER + PAYLOAD_MAX + 1)
        checked(file_identity(os.fstat(stream.fileno())) == file_identity(opened) ==
                file_identity(path.lstat()), "evidence changed during read")
    return data, before.st_size


def inspect(path, decoder):
    path = Path(path)
    checked(NAME.fullmatch(path.name), "evidence expects an explicit 32-hex .blob filename")
    data, size = read_evidence(path)
    evidence = {"file": path.name, "bytes": size, "scope": "checkpoint-envelope-and-codec-only"}
    if len(data) == size:
        evidence["sha256"] = sha(data).hex()

    def result(status):
        return {**evidence, "status": status}

    if size < HEADER:
        return result("short_header")
    magic, version, kind, owner, key, payload_size, payload_sha, generation = struct.unpack_from(
        "<16sII16s32sQ32s16s", data)
    if (magic, version, kind, owner, key, generation.hex() + ".blob") != (
        b"MMSCacheBlob0001", 1, 5, OWNER, KEY, path.name
    ) or not 376 + 160 <= payload_size <= PAYLOAD_MAX:
        return result("invalid_checkpoint_envelope")
    evidence["declared_payload_bytes"] = payload_size
    if size < HEADER + payload_size:
        return result("short_payload")
    if size != HEADER + payload_size:
        return result("trailing_bytes")
    if sha(data[HEADER:]) != payload_sha:
        return result("payload_sha256_mismatch")

    run = subprocess.run([str(decoder)], input=data[HEADER:], capture_output=True, timeout=TIMEOUT)
    if run.returncode == 1:
        return result("checkpoint_codec_rejected")
    checked(run.returncode == 0, "production codec helper failed")
    evidence["decoded"] = json.loads(run.stdout)
    return result("checkpoint_codec_ok")


def wire_ref(name, kind, owner, key, payload_bytes, file_index):
    # Independent byte fixture; neither production Encode nor Decode produces it.
    return struct.pack("<37s3xI4x16s32s32sQI4xQQ", name.encode(), kind, owner, bytes([key]) + bytes(31),
                       sha(name.encode()), payload_bytes, 0x10203040, file_index, HEADER + payload_bytes)


def synthetic_payload(revision):
    project_owner = bytes(range(1, 17))
    snapshot = wire_ref("1" * 32 + ".blob", 3, project_owner, 1, 19, 42)
    previous = wire_ref("2" * 32 + ".blob", 4, OWNER, 2, 368, 43)
    return struct.pack("<16sI4xQ16s", b"MMSCacheBase0001", 1, revision, project_owner) + (
        snapshot + previous + struct.pack("<I4x", 1) + previous
    )


def synthetic_blob(path, payload):
    return struct.pack("<16sII16s32sQ32s16s", b"MMSCacheBlob0001", 1, 5, OWNER, KEY,
                       len(payload), sha(payload), bytes.fromhex(path.stem)) + payload


def sync_write(stream, data):
    checked(stream.write(data) == len(data), "short synthetic write")
    stream.flush()
    os.fsync(stream.fileno())


class PausedWriter:
    """Real isolated process, timed by pipe barriers; no sleeps or changed production logic."""
    CHILD = (
        "import os,sys\n"
        "fd=int(sys.argv[1]); data=bytes.fromhex(sys.argv[2]); offset=0\n"
        "with os.fdopen(fd, 'wb', closefd=False) as out:\n"
        " for limit in (80, 128+256, len(data)):\n"
        "  assert out.write(data[offset:limit])==limit-offset\n"
        "  out.flush(); os.fsync(fd); offset=limit\n"
        "  os.write(1,b'+')\n"
        "  if limit!=len(data) and os.read(0,1)!=b'+': sys.exit(4)\n"
    )

    def __init__(self, path, payload):
        self.path = path
        self.blob = synthetic_blob(path, payload)

    def __enter__(self):
        with self.path.open("xb") as output:
            self.proc = subprocess.Popen([sys.executable, "-B", "-c", self.CHILD, str(output.fileno()), self.blob.hex()],
                                         pass_fds=(output.fileno(),), stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        return self

    def wait(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.proc.stdout, selectors.EVENT_READ)
            checked(selector.select(TIMEOUT) and os.read(self.proc.stdout.fileno(), 1) == b"+",
                    "writer did not reach the write/fsync barrier")

    def advance(self):
        self.proc.stdin.write(b"+")
        self.proc.stdin.flush()

    def finish(self, kill=False):
        if kill:
            self.proc.kill()
        checked(self.proc.wait(TIMEOUT) == (-signal.SIGKILL if kill else 0), "unexpected writer exit")

    def __exit__(self, *_):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(TIMEOUT)
        self.proc.stdin.close()
        self.proc.stdout.close()


def fresh_inspect(path, decoder):
    """A fresh inspector after a writer exit; never reuses reader state."""
    run = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()), "inspect",
                          "--decoder", str(decoder), str(path)], capture_output=True, timeout=TIMEOUT)
    checked(run.returncode in (0, 1), "fresh read-only inspector failed")
    row, = json.loads(run.stdout)
    checked((run.returncode == 0) == (row["status"] == "checkpoint_codec_ok"), "inspect exit mismatch")
    return row


def self_test(decoder):
    outcomes = []
    with tempfile.TemporaryDirectory(prefix="mms-cache-checkpoint-evidence-") as dirname:
        owned = Path(dirname)

        def path(number):
            return owned / (f"{number:032x}.blob")

        def observe(case, evidence, status, sha256=None, revision=None):
            checked(evidence["status"] == status, f"{case}: expected {status}, got {evidence['status']}")
            if sha256 is not None:
                checked(evidence["sha256"] == sha256, f"{case}: input bytes not preserved")
            if revision is not None:
                checked(evidence["decoded"]["revision"] == revision and evidence["decoded"]["retired_count"] == 1 and
                        evidence["decoded"]["previous"]["declared_file_index"] == 43, f"{case}: inconsistent codec output")
            outcomes.append({"case": case, **evidence})

        baseline = path(10)
        baseline_blob = synthetic_blob(baseline, synthetic_payload(10))
        with baseline.open("xb") as stream:
            sync_write(stream, baseline_blob)
        observe("last known valid checkpoint", fresh_inspect(baseline, decoder), "checkpoint_codec_ok",
                sha(baseline_blob).hex(), 10)
        torn = path(11)
        with PausedWriter(torn, synthetic_payload(11)) as writer:
            writer.wait()
            observe("slow writer, short header", fresh_inspect(torn, decoder), "short_header",
                    sha(writer.blob[:80]).hex())
            writer.advance()
            writer.wait()
            partial_sha = sha(writer.blob[:HEADER + 256]).hex()
            observe("slow writer, short payload", fresh_inspect(torn, decoder), "short_payload", partial_sha)
            writer.finish(kill=True)
        observe("fresh inspector after forced writer exit", fresh_inspect(torn, decoder), "short_payload", partial_sha)
        observe("baseline still decodes after writer exit", fresh_inspect(baseline, decoder), "checkpoint_codec_ok",
                sha(baseline_blob).hex(), 10)

        full = path(12)
        bad = bytearray(synthetic_blob(full, synthetic_payload(11)))
        bad[-1] ^= 1
        with full.open("xb") as stream:
            sync_write(stream, bad)
        observe("full-size file with changed payload", fresh_inspect(full, decoder), "payload_sha256_mismatch", sha(bad).hex())
        bad_ref = path(13)
        payload = bytearray(synthetic_payload(11))
        payload[376 + 144] ^= 1  # Change only the retired predecessor's file index.
        data = synthetic_blob(bad_ref, payload)
        with bad_ref.open("xb") as stream:
            sync_write(stream, data)
        observe("valid digest with wrong retirement identity", fresh_inspect(bad_ref, decoder),
                "checkpoint_codec_rejected", sha(data).hex())

        independent = path(14)
        with PausedWriter(independent, synthetic_payload(11)) as writer:
            writer.wait()
            writer.advance()
            writer.wait()
            writer.advance()
            writer.wait()
            writer.finish()
            independent_sha = sha(writer.blob).hex()
        observe("new standalone generation completes all fsync barriers", fresh_inspect(independent, decoder),
                "checkpoint_codec_ok", independent_sha, 11)
        observe("second writer did not clean earlier evidence", fresh_inspect(torn, decoder), "short_payload", partial_sha)

        mislabeled = path(17)
        with mislabeled.open("xb") as stream:
            sync_write(stream, baseline_blob)
        observe("envelope generation disagrees with filename", fresh_inspect(mislabeled, decoder),
                "invalid_checkpoint_envelope", sha(baseline_blob).hex())
        extra = path(18)
        data = synthetic_blob(extra, synthetic_payload(11)) + b"trailing"
        with extra.open("xb") as stream:
            sync_write(stream, data)
        observe("trailing data after valid checkpoint", fresh_inspect(extra, decoder), "trailing_bytes", sha(data).hex())

        untrusted = path(15)
        untrusted.symlink_to(baseline)
        try:
            inspect(untrusted, decoder)
        except RuntimeError as exc:
            checked("non-hardlinked file" in str(exc), "unsafe link rejection did not reach filesystem guard")
        else:
            raise RuntimeError("inspector unexpectedly accepted a symlink")
        outcomes.append({"case": "explicit symlink is not followed", "status": "rejected_by_filesystem_guard"})

        hardlinked = path(16)
        os.link(baseline, hardlinked)
        try:
            inspect(hardlinked, decoder)
        except RuntimeError as exc:
            checked("non-hardlinked file" in str(exc), "unsafe hardlink rejection did not reach filesystem guard")
        else:
            raise RuntimeError("inspector unexpectedly accepted a hardlink")
        outcomes.append({"case": "explicit hardlink is not opened", "status": "rejected_by_filesystem_guard"})

    return {"mode": "linux-synthetic-files-and-real-processes", "cases_passed": len(outcomes),
            "production_codec": "DecodeCheckpoint", "windows_recovery": "not_run", "observations": outcomes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("inspect", help="inspect named preserved .blob files without modifying them")
    check.add_argument("files", type=Path, nargs="+")
    test = commands.add_parser("self-test", help="write and stop isolated synthetic writers in a private temporary root")
    for action in (check, test):
        action.add_argument("--decoder", required=True, type=Path, help="path to the trusted local build of the C++ helper")
    args = parser.parse_args()
    try:
        checked(sys.platform.startswith("linux"), "this diagnostic is scoped to Linux")
        decoder = args.decoder.resolve(strict=True)
        checked(decoder.is_file() and os.access(decoder, os.X_OK), "decoder is not an executable file")
        if args.command == "self-test":
            print(json.dumps(self_test(decoder), indent=2, sort_keys=True))
            return 0
        findings = [inspect(path, decoder) for path in args.files]
        print(json.dumps(findings, indent=2, sort_keys=True))
        return 0 if all(row["status"] == "checkpoint_codec_ok" for row in findings) else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"checkpoint evidence unavailable/inconclusive: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
