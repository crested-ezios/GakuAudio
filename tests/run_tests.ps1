#Requires -Version 5.1
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
[CmdletBinding()]
param(
    [string]$OutputRoot = '',
    [string]$VisualStudioPath = '',
    [string]$WindowsSdkRoot = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$contractCheck = Join-Path $repo 'tools\check_contract_headers.ps1'
if (Test-Path -LiteralPath $contractCheck -PathType Leaf) {
    $global:LASTEXITCODE = 0
    & $contractCheck -RepoRoot $repo
    if ($LASTEXITCODE -ne 0) {
        throw ('Contract header parity check failed: ' + $LASTEXITCODE)
    }
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repo 'build'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$testDir = Join-Path $OutputRoot 'tests'
New-Item -ItemType Directory -Path $testDir -Force | Out-Null

$vs = $VisualStudioPath
if ([string]::IsNullOrWhiteSpace($vs)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$msvc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sdk = $WindowsSdkRoot
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = @(Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'; Join-Path $env:ProgramFiles 'Windows Kits\10') |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Include') } | Select-Object -First 1
}
$sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory |
    Where-Object {
        $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'ucrt\stddef.h') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $sdk "Lib\$($_.Name)\um\x64\advapi32.lib") -PathType Leaf)
    } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1

$env:INCLUDE = @((Join-Path $msvc.FullName 'include'), (Join-Path $sdk "Include\$($sdkVer.Name)\ucrt"),
    (Join-Path $sdk "Include\$($sdkVer.Name)\um"), (Join-Path $sdk "Include\$($sdkVer.Name)\shared"),
    (Join-Path $sdk "Include\$($sdkVer.Name)\winrt")) -join ';'
$env:LIB = @((Join-Path $msvc.FullName 'lib\x64'), (Join-Path $sdk "Lib\$($sdkVer.Name)\ucrt\x64"),
    (Join-Path $sdk "Lib\$($sdkVer.Name)\um\x64")) -join ';'
$env:PATH = (Join-Path $msvc.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$exe = Join-Path $testDir 'test_gakuaudio.exe'
$objDir = Join-Path $testDir 'obj'
New-Item -ItemType Directory -Path $objDir -Force | Out-Null

$vorbisObj = Join-Path $objDir 'stb_vorbis.obj'
& $cl /nologo /c /TC /O2 /MT /W1 /D_CRT_SECURE_NO_WARNINGS (Join-Path $repo 'deps\miniaudio\stb_vorbis.c') "/Fo:$vorbisObj"
if ($LASTEXITCODE -ne 0) { throw "stb_vorbis compile failed" }

$maObj = Join-Path $objDir 'MiniaudioImpl.obj'
& $cl /nologo /c /std:c++20 /utf-8 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS `
    "-I$(Join-Path $repo 'deps\miniaudio')" (Join-Path $repo 'src\MiniaudioImpl.cpp') "/Fo:$maObj"
if ($LASTEXITCODE -ne 0) { throw "MiniaudioImpl compile failed" }

$testObj = Join-Path $objDir 'test_gakuaudio.obj'
& $cl /nologo /std:c++20 /utf-8 /O2 /EHsc /MT /W4 /D_CRT_SECURE_NO_WARNINGS `
    "-I$(Join-Path $repo 'include')" "-I$(Join-Path $repo 'src')" "-I$(Join-Path $repo 'deps\miniaudio')" `
    (Join-Path $PSScriptRoot 'test_gakuaudio.cpp') $maObj $vorbisObj `
    "/Fo:$testObj" '/link' "/OUT:$exe" mfplat.lib mfreadwrite.lib mfuuid.lib propsys.lib ole32.lib winmm.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw "test_gakuaudio compile failed" }

$env:GAKUAUDIO_TEST_DLL = Join-Path $OutputRoot 'native\GakuAudio.dll'
Push-Location -LiteralPath $repo
try {
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "test_gakuaudio failed" }
} finally {
    Remove-Item Env:GAKUAUDIO_TEST_DLL -ErrorAction SilentlyContinue
    Pop-Location
}
