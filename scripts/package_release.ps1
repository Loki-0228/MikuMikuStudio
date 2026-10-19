#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$FullBuild,
    [Parameter(Mandatory)][string]$MinimalBuild,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Version -notmatch '^v\d{4}\.\d{2}\.\d{2}\.\d+(-pre\.\d+)?$') {
    throw '版本格式须为 vYYYY.MM.DD.N 或 vYYYY.MM.DD.N-pre.N。'
}
$isPrerelease = $Version -match '-pre\.\d+$'
$releaseLabelZh = if ($isPrerelease) { '预发布版' } else { '正式版' }
$releaseLabelEn = if ($isPrerelease) { 'prerelease' } else { 'release' }
$releaseLabelJa = if ($isPrerelease) { 'プレリリース版' } else { '正式版' }
$status = & git -C $repo status --porcelain
if ($LASTEXITCODE -ne 0 -or $status) { throw '打包前须提交源码，工作区和暂存区必须干净。' }
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw '无法读取源码提交。' }
$notes = [IO.File]::ReadAllText((Join-Path $repo 'docs/records/releases.md'))
$pattern = '(?s)<!-- (?:pre)?release-notes:' + [regex]::Escape($Version) + ':begin -->\s*(.*?)\s*<!-- (?:pre)?release-notes:' + [regex]::Escape($Version) + ':end -->'
$match = [regex]::Match($notes, $pattern)
if (-not $match.Success) { throw '找不到此版本的发布说明。' }
$releaseNotes = $match.Groups[1].Value + "`n"
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "输出目录已存在，拒绝覆盖：$output" }
$builds = @(
    @{Name='full'; Profile='full'; Path=[IO.Path]::GetFullPath($FullBuild)},
    @{Name='minimal'; Profile='mmd932'; Path=[IO.Path]::GetFullPath($MinimalBuild)}
)
foreach ($build in $builds) {
    $cache = [IO.File]::ReadAllText((Join-Path $build.Path 'CMakeCache.txt'))
    $sourceRoot = [regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Trim()
    $buildProfile = [regex]::Match($cache, '(?m)^MIKUMIKUSTUDIO_PROFILE:STRING=(.+)$').Groups[1].Value.Trim()
    $platform = [regex]::Match($cache, '(?m)^CMAKE_GENERATOR_PLATFORM:STRING=(.+)$').Groups[1].Value.Trim()
    if (-not $sourceRoot -or [IO.Path]::GetFullPath($sourceRoot) -ne $repo -or $buildProfile -ne $build.Profile) {
        throw "构建目录不属于当前源码或构建档错误：$($build.Path)"
    }
    if ($platform -ne 'x64') { throw "发布包要求 x64 构建：$($build.Path)" }
    if (-not (Test-Path -LiteralPath (Join-Path $build.Path 'Release/MikuMikuDance.exe'))) {
        throw "缺少 Release 程序：$($build.Path)"
    }
}
# Build both profiles from the clean commit before copying any binaries.
foreach ($build in $builds) {
    $targets = @('MikuMikuStudio')
    if ($build.Name -eq 'full') { $targets += 'MMSNvidiaAAFix' }
    & cmake --build $build.Path --config Release --target @targets -- /m:2 /nologo
    if ($LASTEXITCODE -ne 0) { throw "构建失败：$($build.Name)" }
}
if ((& git -C $repo rev-parse HEAD).Trim() -ne $commit -or (& git -C $repo status --porcelain)) {
    throw '构建期间源码发生变化，未生成发布包。'
}
New-Item -ItemType Directory -Path $output | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$checksums = [System.Collections.Generic.List[string]]::new()
$packages = [System.Collections.Generic.List[object]]::new()
foreach ($build in $builds) {
    $name = 'MikuMikuStudio-' + $Version + '-windows-x64-' + $build.Name
    $stage = Join-Path $output $name
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -LiteralPath (Join-Path $build.Path 'Release/MikuMikuDance.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'licenses') -Destination $stage -Recurse
    [IO.File]::WriteAllText((Join-Path $stage 'RELEASE_NOTES.md'), $releaseNotes)
    $readme = @"
# MikuMikuStudio $Version ($($build.Name))

Windows x64 ${releaseLabelEn}. Extract full and minimal into separate directories.
Run MikuMikuDance.exe. Keep existing installations that contain settings, models, or plugins.
Neither application package includes the NVIDIA fix. Download MMSNvidiaAAFix.dll separately if needed; see RELEASE_NOTES.md for installation.
See RELEASE_NOTES.md for changes, usage limits, and installation instructions.

Windows x64 ${releaseLabelZh}。解压到独立目录后运行 MikuMikuDance.exe。
完整包与最小包请分别解压。已有设置、模型和插件的安装目录请保留。
两个主程序包均不含英伟达修复插件。需要时单独下载 MMSNvidiaAAFix.dll，安装方法见 RELEASE_NOTES.md。
更新内容、使用限制与安装方法见 RELEASE_NOTES.md，源码提交和文件校验值见 RELEASE_BUILD.json。

Windows x64 ${releaseLabelJa}。完全版と最小版は別々のフォルダーに展開してください。
MikuMikuDance.exe を実行します。設定やモデル、プラグインを含む既存の環境は残してください。
NVIDIA 修正 DLL は両パッケージに含まれません。必要な場合のみ別途ダウンロードし、導入方法は RELEASE_NOTES.md を参照してください。
変更内容、利用上の制限、導入方法は RELEASE_NOTES.md、ソースコミットは RELEASE_BUILD.json を参照してください。

Source: https://github.com/Loki-0228/MikuMikuStudio/tree/$commit
"@
    [IO.File]::WriteAllText((Join-Path $stage 'README.md'), $readme + "`n")
    $files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{Path=[IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\','/'); Bytes=$_.Length; SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    $manifest = [ordered]@{Version=$Version; Commit=$commit; Prerelease=$isPrerelease; Profile=$build.Profile; Platform='windows-x64'; Configuration='Release'; Uploaded=$false; CreatedUtc=[DateTime]::UtcNow.ToString('o'); Files=$files}
    [IO.File]::WriteAllText((Join-Path $stage 'RELEASE_BUILD.json'), ($manifest | ConvertTo-Json -Depth 6) + "`n")
    $zip = Join-Path $output ($name + '.zip')
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksums.Add($hash + '  ' + [IO.Path]::GetFileName($zip))
    $packages.Add([ordered]@{File=[IO.Path]::GetFileName($zip); Bytes=(Get-Item -LiteralPath $zip).Length; SHA256=$hash; Profile=$build.Profile})
}
# Ship the optional helper as its own release asset, never inside either ZIP.
$helper = Join-Path $output 'MMSNvidiaAAFix.dll'
Copy-Item -LiteralPath (Join-Path $builds[0].Path 'Release/MMSNvidiaAAFix.dll') -Destination $helper
$helperHash = (Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash.ToLowerInvariant()
$optionalComponents = @([ordered]@{File='MMSNvidiaAAFix.dll'; Bytes=(Get-Item -LiteralPath $helper).Length; SHA256=$helperHash; SourceProfile='full'; Optional=$true})
$checksums.Add($helperHash + '  MMSNvidiaAAFix.dll')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $output
$checksums.Add((Get-FileHash -LiteralPath (Join-Path $output 'LICENSE') -Algorithm SHA256).Hash.ToLowerInvariant() + '  LICENSE')
[IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS.txt'), $checksums)
[IO.File]::WriteAllText((Join-Path $output 'RELEASE_NOTES.md'), $releaseNotes)
[IO.File]::WriteAllText((Join-Path $output 'RELEASE_BUILD.json'), ([ordered]@{Version=$Version; Commit=$commit; Prerelease=$isPrerelease; Uploaded=$false; Packages=$packages; OptionalComponents=$optionalComponents} | ConvertTo-Json -Depth 6) + "`n")
Write-Output "发布包已生成：$output"
