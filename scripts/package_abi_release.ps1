#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/cmake',
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
$profile = [regex]::Match($cache, '(?m)^MIKUMIKUSTUDIO_PROFILE:STRING=(.+)$').Groups[1].Value.Trim()
$platform = [regex]::Match($cache, '(?m)^CMAKE_GENERATOR_PLATFORM:STRING=(.+)$').Groups[1].Value.Trim()
if (-not $sourceRoot -or [IO.Path]::GetFullPath($sourceRoot) -ne $repo) {
    throw "构建目录不属于当前源码：$build"
}
if ($profile -ne 'full') { throw "发布目录要求完整档构建，当前 MIKUMIKUSTUDIO_PROFILE 为 '$profile'：$build" }
if ($platform -ne 'x64') { throw "发布目录要求 x64 构建：$build" }
$exe = Join-Path $build 'Release/MikuMikuDance.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "缺少 Release 程序：$exe" }
$legacyMmdRelativePath = 'legacy-mmdplugin/MMDPlugin.dll'
$legacyMmdProvider = Join-Path $build "Release/$legacyMmdRelativePath"
if (-not (Test-Path -LiteralPath $legacyMmdProvider -PathType Leaf)) {
    throw "缺少旧版 MMDPlugin 兼容 DLL：$legacyMmdProvider"
}

# The release directory holds the shipped files only. Anything else there
# belongs to the build tree or the verification evidence.
if ((Test-Path -LiteralPath $release) -and @(Get-ChildItem -LiteralPath $release -Force).Count -gt 0) {
    if (-not $Force) { throw "发布目录非空，确认要重建时加 -Force：$release" }
    Remove-Item -LiteralPath $release -Recurse -Force
}
New-Item -ItemType Directory -Path $release -Force | Out-Null

# Keep the provider in the host's expected private directory.
# The build tree and verification evidence stay out of the release.
Copy-Item -LiteralPath $exe -Destination (Join-Path $release 'MikuMikuDance.exe')
$legacyMmdDestination = Join-Path $release $legacyMmdRelativePath
New-Item -ItemType Directory -Path (Split-Path -Parent $legacyMmdDestination) | Out-Null
Copy-Item -LiteralPath $legacyMmdProvider -Destination $legacyMmdDestination
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

$expected = @('legacy-mmdplugin', 'licenses', 'MikuMikuDance.exe', 'mms-adaptive-ui-template.zip')
$actual = @(Get-ChildItem -LiteralPath $release -Force | ForEach-Object Name)
$difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
if ($difference.Count -gt 0) {
    $names = ($difference | ForEach-Object InputObject) -join '、'
    throw "发布目录只能包含程序、旧版 MMDPlugin 兼容 DLL、licenses 和 GUI 模块压缩包，发现多余或缺少：$names"
}

foreach ($name in @('MikuMikuDance.exe', $legacyMmdRelativePath, 'mms-adaptive-ui-template.zip')) {
    $path = Join-Path $release $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output ("{0}  {1}  {2}" -f $hash, (Get-Item -LiteralPath $path).Length, $name)
}
Write-Output "发布目录已生成：$release"
