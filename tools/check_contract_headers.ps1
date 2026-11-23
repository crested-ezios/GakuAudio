#Requires -Version 5.1
# SPDX-License-Identifier: MIT
# NOTE: Keep this script ASCII-only (Windows PowerShell 5.1 ANSI parsing).
#
# tools/check_contract_headers.ps1 -- verify SHA256 parity of vendored
# GakuLoader public SDK contract headers against the sibling GakuLoader checkout.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$LoaderIncludeRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$localInclude = Join-Path $RepoRoot 'include'

if ([string]::IsNullOrWhiteSpace($LoaderIncludeRoot)) {
    $LoaderIncludeRoot = [IO.Path]::GetFullPath((Join-Path $RepoRoot '..\..\GakuLoader\dev\include'))
} else {
    $LoaderIncludeRoot = [IO.Path]::GetFullPath($LoaderIncludeRoot)
}

$headers = @(
    'GakuPlugin.h',
    'GakuPluginMetadata.h',
    'GakuPaths.h',
    'GakuPaths.hpp'
)

$failures = 0
$checked = 0

if (-not (Test-Path -LiteralPath $LoaderIncludeRoot -PathType Container)) {
    Write-Host ("  SKIP: GakuLoader checkout not found at {0}" -f $LoaderIncludeRoot) -ForegroundColor Yellow
    foreach ($header in $headers) {
        $localFile = Join-Path $localInclude $header
        if (-not (Test-Path -LiteralPath $localFile -PathType Leaf)) {
            Write-Host ("  MISSING (GakuAudio): include\{0}" -f $header) -ForegroundColor Red
            $failures++
        }
    }
    if ($failures -ne 0) {
        Write-Host ("Contract header check FAILED ({0} missing local header(s))." -f $failures) -ForegroundColor Red
        exit 1
    }
    Write-Host "Contract header parity skipped (absent GakuLoader checkout)."
    exit 0
}

foreach ($header in $headers) {
    $localFile = Join-Path $localInclude $header
    $siblingFile = Join-Path $LoaderIncludeRoot $header
    if (-not (Test-Path -LiteralPath $localFile -PathType Leaf)) {
        Write-Host ("  MISSING (GakuAudio): include\{0}" -f $header) -ForegroundColor Red
        $failures++
        continue
    }
    if (-not (Test-Path -LiteralPath $siblingFile -PathType Leaf)) {
        Write-Host ("  MISSING (GakuLoader): {0}" -f $siblingFile) -ForegroundColor Red
        $failures++
        continue
    }

    $localHash = (Get-FileHash -LiteralPath $localFile -Algorithm SHA256).Hash
    $siblingHash = (Get-FileHash -LiteralPath $siblingFile -Algorithm SHA256).Hash
    $checked++
    if ($localHash -ne $siblingHash) {
        Write-Host ("  DRIFT: {0} (GakuLoader)" -f $header) -ForegroundColor Red
        Write-Host ("    GakuAudio  : {0}" -f $localHash)
        Write-Host ("    GakuLoader : {0}" -f $siblingHash)
        $failures++
        continue
    }
    Write-Host ("  OK: {0} (GakuLoader: {1})" -f $header, $localHash.Substring(0, 16)) -ForegroundColor Green
}

if ($failures -ne 0) {
    Write-Host ("Contract header parity check FAILED ({0} issue(s))." -f $failures) -ForegroundColor Red
    exit 1
}

Write-Host ("Contract header parity passed ({0} file(s) compared)." -f $checked)
exit 0
