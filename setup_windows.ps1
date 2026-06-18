# ================================================================
#  CompressorStudio — Windows SETUP
#  Run this ONCE as Administrator before the first build
#  Right-click setup_windows.ps1 -> "Run with PowerShell as Admin"
# ================================================================

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

Write-Host ""
Write-Host "================================================"
Write-Host "   CompressorStudio — Windows Setup"
Write-Host "================================================"
Write-Host ""

# ── Step 1: Chocolatey ───────────────────────────────────────────
Write-Host "[1/5] Checking Chocolatey..."
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
    Write-Host "      Installing Chocolatey..."
    Set-ExecutionPolicy Bypass -Scope Process -Force
    [System.Net.ServicePointManager]::SecurityProtocol = `
        [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
    iex ((New-Object System.Net.WebClient).DownloadString(
        'https://community.chocolatey.org/install.ps1'))
    Write-Host "      Chocolatey installed."
} else {
    Write-Host "      Chocolatey already installed. OK"
}

# ── Step 2: cmake, git, mingw ────────────────────────────────────
Write-Host "[2/5] Installing cmake, git, mingw..."
choco install cmake git mingw -y --no-progress
# Refresh PATH so new tools are available
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + `
            [System.Environment]::GetEnvironmentVariable("Path","User")
Write-Host "      cmake, git, mingw installed. OK"

# ── Step 3: vcpkg ────────────────────────────────────────────────
Write-Host "[3/5] Checking vcpkg..."
if (-not (Test-Path "C:\vcpkg")) {
    Write-Host "      Cloning vcpkg to C:\vcpkg..."
    git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
    & C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
    Write-Host "      vcpkg installed."
} else {
    Write-Host "      vcpkg already present at C:\vcpkg. OK"
}

# ── Step 4: SDL2 ─────────────────────────────────────────────────
Write-Host "[4/5] Installing SDL2 via vcpkg..."
& C:\vcpkg\vcpkg install sdl2:x64-mingw-dynamic
& C:\vcpkg\vcpkg integrate install
Write-Host "      SDL2 installed. OK"

# ── Step 5: ImGui ────────────────────────────────────────────────
Write-Host "[5/5] Checking ImGui in libs\imgui..."
$ImguiDir = Join-Path $ScriptDir "libs\imgui"
if (-not (Test-Path (Join-Path $ImguiDir "imgui.h"))) {
    Write-Host "      Cloning ImGui into libs\imgui..."
    New-Item -ItemType Directory -Force -Path (Join-Path $ScriptDir "libs") | Out-Null
    git clone https://github.com/ocornut/imgui.git $ImguiDir
    Write-Host "      ImGui cloned. OK"
} else {
    Write-Host "      ImGui already present. OK"
}

Write-Host ""
Write-Host "================================================"
Write-Host "   Setup complete!"
Write-Host "   Now run:  .\run_windows.ps1"
Write-Host "================================================"
Write-Host ""
