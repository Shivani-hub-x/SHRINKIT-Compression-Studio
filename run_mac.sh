#!/bin/bash
# ================================================================
#  CompressorStudio — macOS BUILD & RUN
#  Run this every time you want to build and launch the app
# ================================================================
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo ""
echo "================================================"
echo "   CompressorStudio — Build & Run (macOS)"
echo "================================================"
echo ""

# ── Check ImGui ───────────────────────────────────────────────────
if [ ! -f "$SCRIPT_DIR/libs/imgui/imgui.h" ]; then
    echo "ERROR: libs/imgui not found."
    echo "       Please run ./setup_mac.sh first."
    exit 1
fi

# ── Check SDL2 ────────────────────────────────────────────────────
if ! brew list sdl2 &>/dev/null; then
    echo "ERROR: SDL2 not found."
    echo "       Please run ./setup_mac.sh first."
    exit 1
fi

# ── Configure ────────────────────────────────────────────────────
echo "[1/3] Configuring build..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
echo "      Configuration done."

# ── Build ─────────────────────────────────────────────────────────
echo "[2/3] Building..."
cmake --build build --config Release
echo "      Build done."

# ── Run ───────────────────────────────────────────────────────────
APP="$SCRIPT_DIR/build/CompressorStudio.app/Contents/MacOS/CompressorStudio"
if [ ! -f "$APP" ]; then
    echo "ERROR: App not found at $APP"
    echo "       Build may have failed — check output above."
    exit 1
fi

echo "[3/3] Launching CompressorStudio..."
echo ""
"$APP"
