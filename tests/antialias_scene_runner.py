"""Compare AA modes on an existing PMM/effect through the hidden real-MME host.

All generated settings, EMM mappings and captures live in --output. Source
PMM/PMX/effect files are read only. No effect or model assets are redistributed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for name in ("executable", "mme", "project", "model", "effect", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--missing-dx12", action="store_true")
    args = parser.parse_args()
    source = [args.project.resolve(), args.model.resolve(), args.effect.resolve()]
    before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in source]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = output / "MikuMikuDance.exe"
    shutil.copy2(args.executable, executable)
    for name in ("d3d9.dll", "MMEffect.dll", "MMHack.dll"):
        shutil.copy2(args.mme / name, output / name)
    (output / "Data").mkdir(exist_ok=True)
    (output / "Data/mmconfig.ini").write_text(
        "40\n40\n1000\n740\n0\n640\n480\n250\n2\n", encoding="ascii")
    mapping = output / "assignment.pmm"
    mapping.write_bytes(b"Explicit MME assignment notification\n")
    text = (f"[Info]\nVersion = 3\n[Object]\nPmd1 = {source[1]}\n"
            f"[Effect]\nDefault = none\nPmd1 = {source[2]}\nPmd1.show = true\n")
    # The native MME parser rejects a UTF-8 BOM; the host resolves UTF-8 paths.
    mapping.with_suffix(".emm").write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(output / "Local"), APPDATA=str(output / "Roaming"),
               MIKUMIKUSTUDIO_AUTOSAVE_CONFIG=str(output / "autosave.ini"),
               MMS_ANTIALIAS_CONFIG=str(output / "antialias.ini"), MMS_AA_EMM=str(mapping),
               MMS_NVIDIA_AA_FIX="0")
    if args.missing_dx12:
        env["MMS_SURFACE_MISSING_DX12"] = "1"
    with (output / "result.log").open("wb") as log:
        result = subprocess.run([str(executable), str(source[0]), "--antialias-scene", str(output)],
                                cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW, timeout=180)
        if result.returncode == 0:
            result = subprocess.run([str(executable), str(source[0]), "--antialias-preference"],
                                    cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=60)
    after = [hashlib.sha256(p.read_bytes()).hexdigest() for p in source]
    summary = {"exit_code": result.returncode, "sources_unchanged": before == after,
               "missing_dx12": args.missing_dx12, "log": str(output / "result.log")}
    (output / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)
    print((output / "result.log").read_text(encoding="utf-8", errors="replace"), flush=True)
    # Pillow is optional; raw BGRA + dimensions are always retained.
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image:
        for raw in output.glob("*.bgra"):
            size = tuple(map(int, raw.with_suffix(".txt").read_text().split()))
            Image.frombytes("RGB", size, raw.read_bytes(), "raw", "BGRX").save(raw.with_suffix(".png"))
    return int(result.returncode != 0 or before != after)


if __name__ == "__main__":
    raise SystemExit(main())
