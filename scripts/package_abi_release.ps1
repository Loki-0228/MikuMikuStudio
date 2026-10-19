#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/win7-x64/build',
    [string]$ReleaseDirectory = 'build/release',
    [string]$UiTemplateArchive = 'build/packages/mms-adaptive-ui-template.zip',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Resolve-PathFromRepo([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repo $Path))
}

$build = Resolve-PathFromRepo $BuildDirectory
$release = Resolve-PathFromRepo $ReleaseDirectory
$template = Resolve-PathFromRepo $UiTemplateArchive
$cachePath = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath)) { throw "缺少 CMake 缓存：$cachePath" }
$cache = [IO.File]::ReadAllText($cachePath)
$sourceRoot = [regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Trim()
$profile = [regex]::Match($cache, '(?m)^MIKUDANCESTUDIO_PROFILE:STRING=(.+)$').Groups[1].Value.Trim()
$platform = [regex]::Match($cache, '(?m)^CMAKE_GENERATOR_PLATFORM:STRING=(.+)$').Groups[1].Value.Trim()
if (-not $sourceRoot -or [IO.Path]::GetFullPath($sourceRoot) -ne $repo) {
    throw "构建目录不属于当前源码：$build"
}
if ($profile -ne 'full') { throw "发布目录要求完整档构建，当前 MIKUDANCESTUDIO_PROFILE 为 '$profile'：$build" }
if ($platform -ne 'x64') { throw "发布目录要求 x64 构建：$build" }
$exe = Join-Path $build 'Release/MikuMikuDance.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "缺少 Release 程序：$exe" }

# The release directory holds the shipped files only. Anything else there
# belongs to the build tree or the verification evidence.
if ((Test-Path -LiteralPath $release) -and @(Get-ChildItem -LiteralPath $release -Force).Count -gt 0) {
    if (-not $Force) { throw "发布目录非空，确认要重建时加 -Force：$release" }
    Remove-Item -LiteralPath $release -Recurse -Force
}
New-Item -ItemType Directory -Path $release -Force | Out-Null

# Copy only the three shipped items. The build tree stays under
# build/win7-x64/build and the verification evidence under build/verification.
Copy-Item -LiteralPath $exe -Destination (Join-Path $release 'MikuMikuDance.exe')
Copy-Item -LiteralPath (Join-Path $repo 'licenses') -Destination $release -Recurse

# The GUI module source package is the only zip in the release directory.
if (-not (Test-Path -LiteralPath $template)) {
    $packager = Join-Path $repo 'tools/package-adaptive-ui-template.py'
    $packagerArguments = @($packager)
    if (Test-Path -LiteralPath (Join-Path $repo 'build/packages/mms-adaptive-ui-template')) {
        $packagerArguments += '--refresh'
    }
    & python @packagerArguments
    if ($LASTEXITCODE -ne 0) { throw "生成 GUI 模块压缩包失败：$template" }
}
Copy-Item -LiteralPath $template -Destination (Join-Path $release 'mms-adaptive-ui-template.zip')

$expected = @('licenses', 'MikuMikuDance.exe', 'mms-adaptive-ui-template.zip')
$actual = @(Get-ChildItem -LiteralPath $release -Force | ForEach-Object Name)
$difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
if ($difference.Count -gt 0) {
    $names = ($difference | ForEach-Object InputObject) -join '、'
    throw "发布目录只能包含程序、licenses 和 GUI 模块压缩包，发现多余或缺少：$names"
}

foreach ($name in @('MikuMikuDance.exe', 'mms-adaptive-ui-template.zip')) {
    $path = Join-Path $release $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output ("{0}  {1}  {2}" -f $hash, (Get-Item -LiteralPath $path).Length, $name)
}
Write-Output "发布目录已生成：$release"
