# Build and run the MPP decoder mocked-library happy-path test.
#
# Usage (from RK3588S_CarTerminal root):
#   powershell -ExecutionPolicy Bypass -File .\tests\test_mpp_decoder_mock.ps1

$ErrorActionPreference = "Stop"

$repoRoot = $MyInvocation.MyCommand.Path | Split-Path -Parent | Split-Path -Parent
$exe      = Join-Path $repoRoot "tests\test_mpp_decoder_mock.exe"

Write-Host "[mpp_decoder_mock] building..." -ForegroundColor Cyan

$compiler = $null
foreach ($c in @("g++", "clang++")) {
    $found = Get-Command $c -ErrorAction SilentlyContinue
    if ($found) { $compiler = $c; break }
}
if (-not $compiler) {
    Write-Host "[mpp_decoder_mock] SKIP: no C++ compiler (g++/clang++) found on PATH" -ForegroundColor Yellow
    exit 0
}

& $compiler -std=c++17 -Wall -Wextra -Wpedantic `
    "-I$(Join-Path $repoRoot 'tests\stubs')" `
    "-I$(Join-Path $repoRoot 'common')" `
    "-I$(Join-Path $repoRoot 'media')" `
    (Join-Path $repoRoot "tests\test_mpp_decoder_mock.cpp") `
    (Join-Path $repoRoot "tests\stubs\dlfcn_mpp_decoder_mock.cpp") `
    (Join-Path $repoRoot "media\mpp_decoder.cpp") `
    (Join-Path $repoRoot "common\logmanager.cpp") `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "[mpp_decoder_mock] BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "[mpp_decoder_mock] running..." -ForegroundColor Cyan
& $exe
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    Write-Host "[mpp_decoder_mock] PASS" -ForegroundColor Green
} else {
    Write-Host "[mpp_decoder_mock] FAIL (exit $rc)" -ForegroundColor Red
}
exit $rc
