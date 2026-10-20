# Build and run the MPP decoder ABI soft-fail tests.
#
# Links the decoder against the host dlfcn stub (dlopen returns nullptr) so the
# soft-fail path is exercised without librockchip_mpp.so or RK3588S hardware.
#
# Usage (from RK3588S_CarTerminal root):
#   powershell -ExecutionPolicy Bypass -File .\tests\test_mpp_decoder_abi.ps1
#
# Exits 0 on success, 1 on test failure or build error.

$ErrorActionPreference = "Stop"

$repoRoot = $MyInvocation.MyCommand.Path | Split-Path -Parent | Split-Path -Parent
$exe      = Join-Path $repoRoot "tests\test_mpp_decoder_abi.exe"

Write-Host "[mpp_decoder_abi] building..." -ForegroundColor Cyan

$compiler = $null
foreach ($c in @("g++", "clang++")) {
    $found = Get-Command $c -ErrorAction SilentlyContinue
    if ($found) { $compiler = $c; break }
}
if (-not $compiler) {
    Write-Host "[mpp_decoder_abi] SKIP: no C++ compiler (g++/clang++) found on PATH" -ForegroundColor Yellow
    exit 0
}

& $compiler -std=c++17 -Wall -Wextra -Wpedantic `
    "-I$(Join-Path $repoRoot 'tests\stubs')" `
    "-I$(Join-Path $repoRoot 'common')" `
    "-I$(Join-Path $repoRoot 'media')" `
    (Join-Path $repoRoot "tests\test_mpp_decoder_abi.cpp") `
    (Join-Path $repoRoot "tests\stubs\dlfcn_stub.cpp") `
    (Join-Path $repoRoot "media\mpp_decoder.cpp") `
    (Join-Path $repoRoot "common\logmanager.cpp") `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "[mpp_decoder_abi] BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "[mpp_decoder_abi] running..." -ForegroundColor Cyan
& $exe
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    Write-Host "[mpp_decoder_abi] PASS" -ForegroundColor Green
} else {
    Write-Host "[mpp_decoder_abi] FAIL (exit $rc)" -ForegroundColor Red
}
exit $rc
