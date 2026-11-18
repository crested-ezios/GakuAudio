#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [ValidateSet('Runtime', 'Tests', 'All')]
    [string]$Target = 'All',
    [string]$OutputRoot = '',
    [string]$WindowsSdkRoot = '',
    [string]$VisualStudioPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)

$vs = $VisualStudioPath
if ([string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found.' }
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ([string]::IsNullOrWhiteSpace($vs)) { throw 'MSVC x64 toolset not found.' }

$pathValue = (($env:Path -split ';') | Where-Object { $_ } | Select-Object -Unique) -join ';'
foreach ($key in @('PATH', 'Path', 'path')) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
$env:Path = $pathValue

if ($Target -eq 'Runtime' -or $Target -eq 'All') {
    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
    $toolset = if ([IO.File]::Exists((Join-Path $vs 'MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145\Toolset.props'))) { 'v145' } else { 'v143' }
    $nativeOutput = Join-Path $OutputRoot 'native'
    $nativeObjects = Join-Path $OutputRoot 'obj\native'
    New-Item -ItemType Directory -Path $nativeOutput, $nativeObjects -Force | Out-Null
    & $msbuild (Join-Path $repo 'GakuAudio.vcxproj') '/p:Configuration=Release' '/p:Platform=x64' "/p:PlatformToolset=$toolset" `
        "/p:OutDir=$nativeOutput/" "/p:IntDir=$nativeObjects/" '/nologo' '/verbosity:minimal'
    if ($LASTEXITCODE -ne 0) { throw ('GakuAudio runtime build failed: ' + $LASTEXITCODE) }
}

if ($Target -eq 'Tests' -or $Target -eq 'All') {
    & (Join-Path $repo 'tests\run_tests.ps1') -OutputRoot $OutputRoot -VisualStudioPath $vs -WindowsSdkRoot $WindowsSdkRoot
    if ($LASTEXITCODE -ne 0) { throw ('GakuAudio tests failed: ' + $LASTEXITCODE) }
}
