# Build and run the weather_config security tests.
#
# Usage (from RK3588S_CarTerminal root):
#   powershell -ExecutionPolicy Bypass -File .\tests\weather_config\build_and_run.ps1
#
# Exits 0 on success, 1 on test failure or build error.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$testDir  = Join-Path $repoRoot "tests\weather_config"
$src      = Join-Path $repoRoot "gui\Weather\weatherconfig.cpp"
$exe      = Join-Path $testDir "test_weather_config.exe"

Write-Host "[weather_config] building..." -ForegroundColor Cyan

# Locate a C++ compiler. Prefer g++ (board host toolchain), fall back to clang++.
$compiler = $null
foreach ($c in @("g++", "clang++")) {
    $found = Get-Command $c -ErrorAction SilentlyContinue
    if ($found) { $compiler = $c; break }
}
if (-not $compiler) {
    Write-Host "[weather_config] SKIP: no C++ compiler (g++/clang++) found on PATH" -ForegroundColor Yellow
    exit 0
}

& $compiler -std=c++17 -Wall -Wextra `
    (Join-Path $testDir "test_weather_config.cpp") `
    $src `
    -o $exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "[weather_config] BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "[weather_config] running..." -ForegroundColor Cyan
& $exe
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    Write-Host "[weather_config] PASS" -ForegroundColor Green
} else {
    Write-Host "[weather_config] FAIL (exit $rc)" -ForegroundColor Red
}
exit $rc
