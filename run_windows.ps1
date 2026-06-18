# ================================================================
#  CompressorStudio — Windows BUILD & RUN
#  Run this every time you want to build and launch the app
#  Open PowerShell inside the project folder, then run:
#    .\run_windows.ps1
# ================================================================

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

Write-Host ""
Write-Host "================================================"
Write-Host "   CompressorStudio — Build & Run (Windows)"
Write-Host "================================================"
Write-Host ""

# ── Check ImGui ───────────────────────────────────────────────────
$ImguiH = Join-Path $ScriptDir "libs\imgui\imgui.h"
if (-not (Test-Path $ImguiH)) {
    Write-Host "ERROR: libs\imgui not found."
    Write-Host "       Please run .\setup_windows.ps1 first."
    exit 1
}

# ── Check vcpkg ───────────────────────────────────────────────────
if (-not (Test-Path "C:\vcpkg\scripts\buildsystems\vcpkg.cmake")) {
    Write-Host "ERROR: vcpkg not found at C:\vcpkg."
    Write-Host "       Please run .\setup_windows.ps1 first."
    exit 1
}

# ── Refresh PATH so cmake and mingw are available ─────────────────
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + `
            [System.Environment]::GetEnvironmentVariable("Path","User") + ";" + `
            "C:\ProgramData\chocolatey\bin;" + `
            "C:\Program Files\CMake\bin"

# ── Configure ────────────────────────────────────────────────────
Write-Host "[1/3] Configuring build..."
cmake -B build `
    -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
    -Wno-dev
Write-Host "      Configuration done."

# ── Build ─────────────────────────────────────────────────────────
Write-Host "[2/3] Building..."
cmake --build build --config Release
Write-Host "      Build done."

# ── Run ───────────────────────────────────────────────────────────
$EXE = Join-Path $ScriptDir "build\CompressorStudio.exe"
if (-not (Test-Path $EXE)) {
    Write-Host "ERROR: Executable not found at $EXE"
    Write-Host "       Build may have failed — check output above."
    exit 1
}

Write-Host "[3/3] Launching CompressorStudio..."
Write-Host ""
& $EXE
