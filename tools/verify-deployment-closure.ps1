# Deployment closure audit for the MikuMikuStudio Windows package.
#
#   pwsh -File tools/verify-deployment-closure.ps1 -DeploymentDir <dir> [-ReportPath <json>]
#
# The script reads the PE import tables with dumpbin and classifies every
# dependency as one of:
#   system     - shipped by Windows 7 SP1 x64 itself;
#   redist     - VC++/UCRT/OpenMP runtime that must sit beside the EXE;
#   directx    - DirectX 9 redistributable DLLs that must sit beside the EXE;
#   prohibited - APIs deliberately unavailable on the Win7 baseline.
# Unknown DLLs and missing redistributable/directx files fail the audit.
param(
    [Parameter(Mandatory = $true)][string]$DeploymentDir,
    [string]$ReportPath = "",
    [string]$Dumpbin = ""
)
$ErrorActionPreference = 'Stop'
$deployment = (Resolve-Path -LiteralPath $DeploymentDir).Path
if (-not $ReportPath) { $ReportPath = Join-Path $deployment 'deployment-closure.json' }

if (-not $Dumpbin) {
    $candidates = @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'Hostx64\\x64' } | Sort-Object FullName -Descending)
    if ($candidates.Count -eq 0) { throw 'dumpbin.exe not found; pass -Dumpbin' }
    $Dumpbin = $candidates[0].FullName
}

$system = @(
    'kernel32.dll','user32.dll','gdi32.dll','advapi32.dll','shell32.dll','ole32.dll',
    'oleaut32.dll','comdlg32.dll','comctl32.dll','winmm.dll','dsound.dll','d3d9.dll',
    'gdiplus.dll','mfplat.dll','mfreadwrite.dll','msimg32.dll','msvfw32.dll','avifil32.dll',
    'bcrypt.dll','shlwapi.dll','ws2_32.dll','wininet.dll','winhttp.dll','version.dll',
    'imm32.dll','setupapi.dll','cfgmgr32.dll','msacm32.dll','avicap32.dll','oleacc.dll',
    'uxtheme.dll','dwmapi.dll','dwrite.dll','propsys.dll','ncrypt.dll','crypt32.dll','msvcrt.dll',
    'secur32.dll','iphlpapi.dll','dnsapi.dll','userenv.dll','psapi.dll','powrprof.dll'
)
$redistPatterns = @('msvcp140*.dll','vcruntime140*.dll','concrt140*.dll','vcomp140*.dll',
    'ucrtbase.dll','api-ms-win-crt-*.dll','api-ms-win-core-*.dll')
$directx = @('d3dx9_43.dll','d3dcompiler_43.dll')
$prohibited = @('d3d12.dll','dxgi.dll','d3d11.dll','d3d10.dll','d3dcompiler_47.dll',
    'cuda.dll','nvcuda.dll','nvapi64.dll','nvapi.dll')

function Get-Imports([string]$binary) {
    $out = Join-Path ([IO.Path]::GetTempPath()) ('closure-' + [guid]::NewGuid().ToString('N') + '.txt')
    $err = $out + '.err'
    try {
        $arguments = @('/nologo', '/dependents', $binary)
        $process = Start-Process -FilePath $Dumpbin -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        if (-not $process.WaitForExit(60000)) { try { $process.Kill() } catch {}; throw "dumpbin timeout: $binary" }
        $text = Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue
        if (-not $text) { throw "dumpbin produced no output: $binary" }
        $imports = New-Object System.Collections.Generic.List[string]
        $delay = New-Object System.Collections.Generic.List[string]
        $section = ''
        foreach ($line in ($text -split '\r?\n')) {
            if ($line -match 'Image has the following dependencies:') { $section = 'imports'; continue }
            if ($line -match 'Image has the following delay load dependencies:') { $section = 'delay'; continue }
            if ($line -match '^\s*Summary') { $section = ''; continue }
            if (-not $section) { continue }
            $name = $line.Trim()
            if (-not $name -or $name -notmatch '\.dll$') { continue }
            if ($section -eq 'imports') { $imports.Add($name.ToLowerInvariant()) } else { $delay.Add($name.ToLowerInvariant()) }
        }
        return [pscustomobject]@{ Imports = @($imports); DelayLoads = @($delay) }
    } finally {
        Remove-Item -LiteralPath $out, $err -Force -ErrorAction SilentlyContinue
    }
}

function Classify([string]$name) {
    if ($prohibited -contains $name) { return 'prohibited' }
    if ($directx -contains $name) { return 'directx' }
    foreach ($pattern in $redistPatterns) { if ($name -like $pattern) { return 'redist' } }
    if ($system -contains $name) { return 'system' }
    return 'unknown'
}

$shipped = @{}
Get-ChildItem -LiteralPath $deployment -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.dll', '.exe' } |
    ForEach-Object { $shipped[$_.Name.ToLowerInvariant()] = $true }

$files = New-Object System.Collections.Generic.List[object]
$problems = New-Object System.Collections.Generic.List[string]
$binaries = Get-ChildItem -LiteralPath $deployment -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object FullName
foreach ($binary in $binaries) {
    $relative = $binary.FullName.Substring($deployment.Length).TrimStart('\')
    $table = Get-Imports $binary.FullName
    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($name in @($table.Imports + $table.DelayLoads)) {
        $class = Classify $name
        $present = $shipped.ContainsKey($name)
        $entry = [pscustomobject]@{ name = $name; class = $class; shipped = $present }
        $entries.Add($entry)
        if ($class -eq 'prohibited') { $problems.Add("$relative imports prohibited $name") }
        elseif ($class -eq 'unknown') { $problems.Add("$relative imports unrecognized $name") }
        elseif (($class -eq 'redist' -or $class -eq 'directx') -and -not $present) {
            $problems.Add("$relative needs $name but the deployment folder does not ship it")
        }
    }
    $files.Add([pscustomobject]@{ file = $relative; imports = $entries.ToArray() })
}

$report = [pscustomobject]@{
    deployment = $deployment
    dumpbin = $Dumpbin
    generatedAt = (Get-Date).ToString('o')
    systemDlls = $system
    redistributableDlls = @($shipped.Keys | Where-Object { (Classify $_) -eq 'redist' } | Sort-Object)
    directXDlls = @($shipped.Keys | Where-Object { (Classify $_) -eq 'directx' } | Sort-Object)
    binaries = $files.ToArray()
    problems = @($problems)
    ok = ($problems.Count -eq 0)
}
$json = $report | ConvertTo-Json -Depth 8
Set-Content -LiteralPath $ReportPath -Value $json -Encoding UTF8

foreach ($binary in $report.binaries) {
    $classes = @{}
    foreach ($entry in $binary.imports) { $classes[$entry.class] = 1 + ($classes[$entry.class] -as [int]) }
    $summary = ($classes.Keys | Sort-Object | ForEach-Object { "$_=$($classes[$_])" }) -join ' '
    Write-Host ("{0}: {1}" -f $binary.file, $summary)
}
if ($report.problems.Count) {
    Write-Host "DEPLOYMENT CLOSURE FAILED"
    $report.problems | ForEach-Object { Write-Host ("  " + $_) }
    exit 1
}
Write-Host ("DEPLOYMENT CLOSURE OK: {0} binaries, {1} redistributable, {2} DirectX files" -f
    $report.binaries.Count, $report.redistributableDlls.Count, $report.directXDlls.Count)
Write-Host ("report: " + $ReportPath)
exit 0
