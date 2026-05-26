#!/usr/bin/env bash
# Cross-compile SurgeMidiToOscBridge for aarch64 Ubuntu/RPi OS (RPi5) from WSL2.
#
# Prerequisites: see docs/cross-compile-linux-aarch64-ubuntu.md
#
# Usage:
#   ./build.sh             # HailoRT voice pipeline ON  (default)
#   ./build.sh --no-hailo  # OSC + Whisper CPU only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SURGE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLCHAIN="$SURGE_DIR/cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake"
BUILD_DIR="$SCRIPT_DIR/build"
NATIVE_BUILD="$BUILD_DIR/native"
CROSS_BUILD="$BUILD_DIR/aarch64-linux"
DIST_DIR="$BUILD_DIR/dist"

HAILO=ON
if [[ "${1:-}" == "--no-hailo" ]]; then
    HAILO=OFF
fi

# ── Step 1: Native juceaide (skip if already built) ───────────────────────────
echo "=== Step 1: Native juceaide ==="
JUCEAIDE_EXE="$(find "$NATIVE_BUILD" -name "juceaide" -type f 2>/dev/null | head -1)"
if [[ -z "$JUCEAIDE_EXE" || ! -x "$JUCEAIDE_EXE" ]]; then
    cmake -S "$SURGE_DIR" -B "$NATIVE_BUILD" \
        -DSURGE_HOST_JUCEAIDE_ONLY=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -G Ninja
    cmake --build "$NATIVE_BUILD"
    JUCEAIDE_EXE="$(find "$NATIVE_BUILD" -name "juceaide" -type f 2>/dev/null | head -1)"
    if [[ -z "$JUCEAIDE_EXE" || ! -x "$JUCEAIDE_EXE" ]]; then
        echo "ERROR: juceaide not found after build"
        exit 1
    fi
else
    echo "  already built — skipping"
fi
echo "  juceaide: $JUCEAIDE_EXE"

# ── Step 2: Cross-compile bridge ─────────────────────────────────────────────
echo ""
echo "=== Step 2: Cross-compile SurgeMidiToOscBridge (Hailo=$HAILO) ==="
cmake -S "$SURGE_DIR" -B "$CROSS_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DJUCE_JUCEAIDE_PATH="$JUCEAIDE_EXE" \
    -DSURGE_HOST_MIDI_OSC_BRIDGE_ONLY=ON \
    -DSURGE_MIDI_OSC_BRIDGE_ENABLE_HAILO="$HAILO" \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja
cmake --build "$CROSS_BUILD" --target SurgeMidiToOscBridge

# ── Step 3: Collect dist ──────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
echo ""
echo "=== Collecting ==="

src="$(find "$CROSS_BUILD" -type f -name "SurgeMidiToOscBridge" ! -path "*/_deps/*" 2>/dev/null | head -1)"
if [[ -n "$src" ]]; then
    cp "$src" "$DIST_DIR/SurgeMidiToOscBridge"
    echo "  → dist/SurgeMidiToOscBridge"
else
    echo "  WARNING: SurgeMidiToOscBridge binary not found in $CROSS_BUILD"
fi

MODELS_DIR="$SURGE_DIR/libs/whisper.cpp/models"
mkdir -p "$DIST_DIR/models"
for model in tiny base small; do
    f="$MODELS_DIR/ggml-${model}.bin"
    if [[ -f "$f" ]]; then
        cp "$f" "$DIST_DIR/models/ggml-${model}.bin"
        echo "  → dist/models/ggml-${model}.bin"
    fi
done

if [[ "$HAILO" == "ON" ]]; then
    HEF_SRC="$SCRIPT_DIR/Resources/models/hailo10h"
    if [[ -d "$HEF_SRC" ]]; then
        mkdir -p "$DIST_DIR/models/hailo10h"
        if cp "$HEF_SRC/"*.hef "$DIST_DIR/models/hailo10h/" 2>/dev/null; then
            echo "  → dist/models/hailo10h/*.hef"
        fi
    fi
fi

echo ""
echo "=== Done ==="
echo "  Run:  python deploy.py --target-ip <pi-ip>"
