"""Generate a PMX 2.0 material-morph fixture and run an isolated host probe.

The model is authored by the test. It does not contain animation keys: the
host probe must register weights before capturing a scene. See pmx() for the
exact model and morph indices. No external plugin is packaged or redistributed.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from run_geometry_probe import text

pack = struct.pack


def pmx():
    # Header: UTF-16LE strings, one additional UV, one-byte vertex/texture/
    # material/bone/morph/rigid indices. The original loader accepts PMX 2.0.
    data = bytearray(b"PMX " + pack("<f", 2.0) + bytes([8, 0, 1, 1, 1, 1, 1, 1, 1]))
    for value in ("Material scene fixture", "Material scene fixture", "", ""):
        data += text(value)

    # Vertices 0,1,2 form material 0; 2,1,3 form material 1. Shared vertices
    # intentionally exercise material segmentation without duplicating points.
    positions = ((-1., 0., 0.), (1., 0., 0.), (-1., 2., 0.), (1., 2., 0.))
    data += pack("<i", len(positions))
    for index, position in enumerate(positions):
        data += pack("<12f", *position, 0., 0., -1., index * .125, index * .25,
                     index + .25, index + .5, index + .75, index + 1.)
        # BDEF1 bone 0, edge scale 1.
        data += pack("<Bbf", 0, 0, 1.)
    data += pack("<i6B", 6, 0, 1, 2, 2, 1, 3)
    data += pack("<i", 1) + text("color.bmp")

    # Two deliberately distinct binding materials. The seven tail fields are
    # texture index, sphere index, sphere mode, shared-toon flag and toon index.
    data += pack("<i", 2)
    material0 = (.8, .7, .6, 1., .1, .2, .3, 8., .4, .3, .2,
                 15, .1, .2, .3, .9, .6, 0, -1, 0, 1, 0)
    material1 = (.3, .4, .5, .75, .35, .25, .15, 16., .2, .1, .05,
                 15, .8, .6, .4, .5, 1.2, 0, -1, 0, 1, 0)
    for index, material in enumerate((material0, material1)):
        data += text("material-" + str(index)) + text("material-" + str(index))
        data += pack("<11fB5fbbBBB", *material)
        data += text("") + pack("<i", 3)

    data += pack("<i", 1) + text("root") + text("root")
    data += pack("<3fbiH3f", 0., 0., 0., -1, 0, 0x1E, 0., 1., 0.)

    # The 28 floats in each material record are, in PMX order:
    # diffuse[4], specular[3], specularPower, ambient[3], edgeColor[4],
    # edgeSize, textureTint[4], sphereTint[4], toonTint[4]. Values differ in
    # every channel so source mix-ups and add/multiply order are observable.
    multiply_all = (
        .75, .5, 1.25, .8, .5, .75, 1.25, .625, 1.1, .8, .7,
        .9, .8, .7, .6, 1.5, .5, .6, .7, .8, .9, 1.1, 1.2, 1.3,
        .4, .5, .6, .7,
    )
    multiply_second = (
        1.2, .9, .7, 1.1, .8, 1.3, .6, 1.4, .7, 1.2, .9,
        1.1, .9, .8, .7, .75, 1.1, .9, .8, .7, .6, .8, 1.1, 1.2,
        1.3, 1.2, .9, .8,
    )
    add_all = (
        .11, -.12, .13, -.14, .15, -.16, .17, -.18, .19, -.20, .21,
        -.22, .23, -.24, .25, -.26, .27, -.28, .29, -.30, .31, -.32, .33, -.34,
        .35, -.36, .37, -.38,
    )
    add_first = (
        -.41, .42, -.43, .44, -.45, .46, -.47, .48, -.49, .50, -.51,
        .52, -.53, .54, -.55, .56, -.57, .58, -.59, .60, -.61, .62, -.63, .64,
        -.65, .66, -.67, .68,
    )
    # PMX morph index 0 = group, 1 = multiply, 2 = add. Direct and group
    # weights may be combined by the host. The duplicate group reference
    # intentionally makes contribution ordering observable.
    data += pack("<i", 3)
    data += text("material-group") + text("material-group") + pack("<BBi", 4, 0, 3)
    data += pack("<bfbfbf", 1, .5, 2, -.25, 1, -.125)
    data += text("material-multiply") + text("material-multiply") + pack("<BBi", 4, 8, 2)
    data += pack("<bB28f", -1, 0, *multiply_all)
    data += pack("<bB28f", 1, 0, *multiply_second)
    data += text("material-add") + text("material-add") + pack("<BBi", 4, 8, 2)
    data += pack("<bB28f", -1, 1, *add_all)
    data += pack("<bB28f", 0, 1, *add_first)
    data += pack("<iii", 0, 0, 0)  # Display frames, rigid bodies, joints.
    return bytes(data)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def public_cases(binary, plugin, output):
    from run_mesh_definition_probe import pmd
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture
    report = dict(binary=str(binary), plugin=str(plugin), cases=[])
    failed = False
    for kind, make_model in (("pmx", pmx), ("pmd", pmd)):
        root = output / kind
        make_fixture(root, 255, [""] * 10)
        for directory in ("data", "temp", "local"):
            (root / directory).mkdir()
        model = root / ("mesh-material-scene." + kind)
        model.write_bytes(make_model())
        package = root / "PLUGINS" / "evaluation"
        package.mkdir(parents=True)
        copied_plugin = package / "mms_evaluation_host_probe.dll"
        shutil.copy2(plugin, copied_plugin)
        manifest = dict(schemaVersion=1, id="org.example.evaluation-material-host", name="Material frame probe",
            version="1.0.0", kind="native", abi=dict(major=1, minMinor=0), entry=copied_plugin.name,
            architecture="x64", minWindows="6.1.7601", capabilities=["scene.evaluate"],
            requiresServices=[dict(id="mms.scene.evaluate", major=1, minMinor=7)],
            dependencies=[], nativeDependencies=[], reloadPolicy="restart")
        (package / "plugin.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        exe = root / "probe.exe"
        shutil.copy2(binary, exe)
        env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model), MMS_PLUGIN_TEST_DIR=str(root / "PLUGINS"),
                   TEMP=str(root / "temp"), TMP=str(root / "temp"), LOCALAPPDATA=str(root / "local"))
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        command = [str(exe), "--mesh-material-scene-only"]
        case = dict(kind=kind, command=command, inputs={str(x):sha256(x) for x in (exe,copied_plugin,model)})
        log_path = root / "result.log"
        with log_path.open("w", encoding="utf-8") as log:
            try:
                result = subprocess.run(command, cwd=root, env=env, startupinfo=startup,
                    creationflags=subprocess.CREATE_NO_WINDOW, stdin=subprocess.DEVNULL,
                    stdout=log, stderr=subprocess.STDOUT, timeout=180)
                case["exitCode"] = result.returncode
            except subprocess.TimeoutExpired:
                case.update(exitCode=124,error="Host probe exceeded 180 seconds")
                log.write("\nFAIL: Host probe exceeded 180 seconds\n")
            except OSError as error:
                case.update(exitCode=125,error=str(error))
                log.write("\nFAIL: " + str(error) + "\n")
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        case["completionMarker"] = "Frozen mesh material scene:" in log_text
        case["publicNegotiation"] = "real plugin negotiates material 1.6 through manager" in log_text
        case["referenceNegotiation"] = "real plugin negotiates references 1.7 through manager" in log_text
        case["referenceLifetime"] = "Frozen material references: real plugin, IDs, paths, toon and independent copies passed" in log_text
        case["logSha256"] = sha256(log_path)
        report["cases"].append(case)
        print(kind, log_text[-8000:], flush=True)
        failed = (bool(case["exitCode"]) or not case["completionMarker"] or not case["publicNegotiation"]
                  or not case["referenceNegotiation"] or not case["referenceLifetime"] or failed)
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    raise SystemExit(1 if failed else 0)


def main():
    if len(sys.argv)==4:
        public_cases(*(Path(value).resolve() for value in sys.argv[1:]))
        return
    binary, output = [Path(value).resolve() for value in sys.argv[1:]]
    # Never reuse or overwrite evidence from another run.
    output.mkdir(exist_ok=False)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from pmd_selection_fixtures import make_fixture

    make_fixture(output, 255, [""] * 10)
    for directory in ("data", "temp", "local"):
        (output / directory).mkdir()
    model = output / "mesh-material-scene.pmx"
    model.write_bytes(pmx())
    exe = output / "probe.exe"
    shutil.copy2(binary, exe)
    env = dict(os.environ, MMS_SCENE_TEST_MODEL=str(model),
               TEMP=str(output / "temp"), TMP=str(output / "temp"),
               LOCALAPPDATA=str(output / "local"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    command = [str(exe), "--mesh-material-scene-only"]
    log_path = output / "result.log"
    case = dict(command=command, exeSha256=sha256(exe), modelSha256=sha256(model),
                scope="PMX 2.0 two materials, group, additive and multiplicative morphs")
    with log_path.open("w", encoding="utf-8") as log:
        try:
            result = subprocess.run(command, cwd=output, env=env, startupinfo=startup,
                                    creationflags=subprocess.CREATE_NO_WINDOW,
                                    stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=180)
            case["exitCode"] = result.returncode
        except subprocess.TimeoutExpired:
            case.update(exitCode=124, error="Host probe exceeded 180 seconds")
            log.write("\nFAIL: Host probe exceeded 180 seconds\n")
        except OSError as error:
            case.update(exitCode=125, error=str(error))
            log.write("\nFAIL: " + str(error) + "\n")
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    case["completionMarker"] = "Frozen mesh material scene:" in log_text
    case["logSha256"] = sha256(log_path)
    (output / "report.json").write_text(json.dumps(case, indent=2), encoding="utf-8")
    print(log_text[-8000:], flush=True)
    raise SystemExit(case["exitCode"] or (0 if case["completionMarker"] else 1))


if __name__ == "__main__":
    main()
