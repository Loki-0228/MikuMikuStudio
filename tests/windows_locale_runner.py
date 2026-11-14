"""Run regressions with real Windows 11 process code pages.

Usage: python tests/windows_locale_runner.py <test.exe> <output-dir> [names|selectors|menu|mme-paths]
For mme-paths, use effect_path_compat_tests.exe; only ACP 932 is run.
Only copied test executables receive a manifest; system settings stay unchanged.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    mode = sys.argv[3] if len(sys.argv) > 3 else 'names'
    modes = {
        'names': (['--name-gaps'], 'Name gap'),
        'selectors': (['--model-selectors'], 'Model selector'),
        'menu': ([], 'Menu'),
        'mme-paths': (['--japanese-path-encoding'], 'Effect path'),
    }
    arguments, marker = modes[mode]
    output.mkdir(parents=True, exist_ok=True)
    kits = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10/bin'
    candidates = sorted(kits.glob('*/x64/mt.exe'), reverse=True)
    if not candidates:
        raise RuntimeError('Windows SDK manifest tool was not found')
    rows = []
    locales = [('ja-JP', 932), ('zh-CN', 936), ('en-US', 1252), ('UTF-8', 65001)]
    for locale, code_page in locales[:1] if mode == 'mme-paths' else locales:
        target = output / f'{mode}-{locale}.exe'
        manifest = output / f'{locale}.manifest'
        shutil.copy2(executable, target)
        manifest.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">'
            '<application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>'
            '<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">'
            + locale + '</activeCodePage></windowsSettings></application></assembly>', encoding='utf-8')
        subprocess.run([str(candidates[0]), '-nologo', '-manifest', str(manifest),
                        f'-outputresource:{target};#1'], check=True, capture_output=True)
        start = time.perf_counter()
        result = subprocess.run([str(target), *arguments], capture_output=True, timeout=30)
        elapsed = time.perf_counter() - start
        text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        (output / f'{locale}.log').write_text(text, encoding='utf-8')
        correct_cp = f'{marker} regression: ACP={code_page}\n' in text.replace('\r\n', '\n')
        row = {'locale': locale, 'expected_code_page': code_page, 'code_page_verified': correct_cp,
               'exit_code': result.returncode, 'seconds': elapsed,
               'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}
        rows.append(row)
        print(json.dumps(row), flush=True)
    (output / 'results.json').write_text(json.dumps({
        'source_executable': str(executable),
        'mode': mode,
        'source_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
        'results': rows}, indent=2), encoding='utf-8')
    return 0 if all(r['exit_code'] == 0 and r['code_page_verified'] for r in rows) else 1


if __name__ == '__main__':
    sys.exit(main())
