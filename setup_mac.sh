#!/bin/bash
# ================================================================
#  CompressorStudio — macOS SETUP
#  Run this ONCE before the first build
# ================================================================
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo ""
echo "================================================"
echo "   CompressorStudio — macOS Setup"
echo "================================================"
echo ""

# ── Step 1: Homebrew ─────────────────────────────────────────────
echo "[1/4] Checking Homebrew..."
if ! command -v brew &>/dev/null; then
    echo "      Installing Homebrew (enter your Mac password when asked)..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    # Add brew to PATH for Apple Silicon Macs
    if [ -f /opt/homebrew/bin/brew ]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
        echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
    fi
else
    echo "      Homebrew already installed. OK"
fi

# ── Step 2: cmake + sdl2 ─────────────────────────────────────────
echo "[2/4] Installing cmake and sdl2..."
brew install cmake sdl2
echo "      cmake and sdl2 installed. OK"

# ── Step 3: git ──────────────────────────────────────────────────
echo "[3/4] Checking git..."
if ! command -v git &>/dev/null; then
    xcode-select --install
    echo "      Installed Xcode Command Line Tools (includes git)."
else
    echo "      git already installed. OK"
fi

# ── Step 4: ImGui ────────────────────────────────────────────────
echo "[4/4] Checking ImGui in libs/imgui..."
if [ ! -d "$SCRIPT_DIR/libs/imgui/imgui.h" ] && [ ! -f "$SCRIPT_DIR/libs/imgui/imgui.h" ]; then
    echo "      Cloning ImGui into libs/imgui..."
    mkdir -p "$SCRIPT_DIR/libs"
    git clone https://github.com/ocornut/imgui.git "$SCRIPT_DIR/libs/imgui"
    echo "      ImGui cloned. OK"
else
    echo "      ImGui already present. OK"
fi

echo ""
echo "================================================"
echo "   Setup complete!"
echo "   Now run:  ./run_mac.sh"
echo "================================================"
echo ""
