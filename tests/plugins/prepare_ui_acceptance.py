"""Prepare an isolated visible UI acceptance copy; never starts or closes an editor."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

binary, plugin, root = (Path(value).resolve() for value in sys.argv[1:])
root.mkdir(parents=True, exist_ok=False)
(root / "data").mkdir()
package = root / "PLUGINS" / "ui-acceptance"
package.mkdir(parents=True)
shutil.copy2(binary, root / "MMS-UI-Acceptance.exe")
shutil.copy2(plugin, package / "lifecycle.dll")
manifest = dict(schemaVersion=1, id="org.example.gui-panel", name="UI acceptance panel", version="1.0.0",
                kind="native", abi=dict(major=1, minMinor=0), entry="lifecycle.dll", architecture="x64",
                minWindows="6.1.7601", capabilities=["ui.command", "ui.element"],
                requiresServices=[dict(id="mms.ui", major=1, minMinor=0)],
                dependencies=[], nativeDependencies=[], reloadPolicy="restart")
(package / "plugin.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
(root / "data" / "settings.json").write_text(json.dumps(dict(schemaVersion=1, sections=dict(
    window=dict(x=40, y=40, width=1100, height=760, maximized=0, renderWidth=960, renderHeight=540,
                sidebarWidth=250, uiLanguage=2),
    history=dict(recordAdjustments=False, steps=32, memoryMiB=64))), indent=2), encoding="utf-8")
record = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              pluginSha256=hashlib.sha256(plugin.read_bytes()).hexdigest(),
              userReportedEnvironment=dict(monitors=1, inputMethod="Microsoft Pinyin"),
              ime="pending user operation", singleMonitorScaling="pending user operation",
              mixedDpiMonitorMove="not available on single monitor", physicalMouse="pending user operation")
(root / "acceptance.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
source = Path(__file__).resolve().parents[2] / "docs" / "records" / "abi-ui-manual-acceptance-2026-10-17.md"
shutil.copy2(source, root / "README.md")
print(root)
