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

# Reuse the Python 3.10+ required by the adaptive UI template packager.
$packager = Join-Path $PSScriptRoot '../tools/package-abi-release.py'
$packagerArguments = @(
    $packager, "--build-directory=$BuildDirectory", "--release-directory=$ReleaseDirectory",
    "--ui-template-archive=$UiTemplateArchive"
)
if ($Force) { $packagerArguments += '--force' }
& python @packagerArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
