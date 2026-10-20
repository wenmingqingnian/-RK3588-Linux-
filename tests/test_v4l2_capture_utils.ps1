# Build and run V4L2 capture utility tests.
#
# Usage (from RK3588S_CarTerminal root):
#   powershell -ExecutionPolicy Bypass -File .\tests\test_v4l2_capture_utils.ps1

$ErrorActionPreference = "Stop"

$repoRoot = $MyInvocation.MyCommand.Path | Split-Path -Parent | Split-Path -Parent
$exe      = Join-Path $repoRoot "tests\test_v4l2_capture_utils.exe"

Write-Host "[v4l2_capture_utils] building..." -ForegroundColor Cyan

$compiler = $null
foreach ($c in @("g++", "clang++")) {
    $found = Get-Command $c -ErrorAction SilentlyContinue
    if ($found) { $compiler = $c; break }
}
if (-not $compiler) {
    Write-Host "[v4l2_capture_utils] SKIP: no C++ compiler (g++/clang++) found on PATH" -ForegroundColor Yellow
    exit 0
}

& $compiler -std=c++17 -Wall -Wextra -Wpedantic `
    "-I$(Join-Path $repoRoot 'media')" `
    (Join-Path $repoRoot "tests\test_v4l2_capture_utils.cpp") `
    (Join-Path $repoRoot "media\v4l2_capture_utils.cpp") `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "[v4l2_capture_utils] BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "[v4l2_capture_utils] running..." -ForegroundColor Cyan
& $exe
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    Write-Host "[v4l2_capture_utils] PASS" -ForegroundColor Green
} else {
    Write-Host "[v4l2_capture_utils] FAIL (exit $rc)" -ForegroundColor Red
}
exit $rc
