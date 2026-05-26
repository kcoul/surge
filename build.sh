#!/usr/bin/env bash
# Cross-compile SurgeXT Standalone for aarch64 Ubuntu/RPi OS (RPi5) from WSL2.
#
# Prerequisites: see extras/SurgeMidiToOscBridge/docs/cross-compile-linux-aarch64-ubuntu-surgeXT.md
# Tip: if you already ran extras/SurgeMidiToOscBridge/build.sh, Step 1 is
# skipped automatically — both scripts share the same native juceaide.
#
# Usage:
#   ./build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JUCE_DIR="$SCRIPT_DIR/libs/JUCE"
TOOLCHAIN="$SCRIPT_DIR/cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake"

# Shared juceaide lives in the bridge's native build so both scripts stay in sync.
BRIDGE_NATIVE="$SCRIPT_DIR/extras/SurgeMidiToOscBridge/build/native"
NATIVE_BUILD="$BRIDGE_NATIVE"

BUILD_DIR="$SCRIPT_DIR/build"
CROSS_BUILD="$BUILD_DIR/aarch64-linux"
DIST_DIR="$BUILD_DIR/dist"

# ── Step 1: Build host juceaide directly from JUCE (shared with bridge) ───────
echo "=== Step 1: Native juceaide ==="
JUCEAIDE_EXE="$(find "$NATIVE_BUILD" -name "juceaide" -type f 2>/dev/null | head -1 || true)"
if [[ -z "$JUCEAIDE_EXE" || ! -x "$JUCEAIDE_EXE" ]]; then
    echo "  configuring JUCE for host (builds juceaide during configure)..."
    cmake -S "$JUCE_DIR" -B "$NATIVE_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -G Ninja
    JUCEAIDE_EXE="$(find "$NATIVE_BUILD" -name "juceaide" -type f 2>/dev/null | head -1 || true)"
    if [[ -z "$JUCEAIDE_EXE" || ! -x "$JUCEAIDE_EXE" ]]; then
        echo "ERROR: juceaide not found after build"
        exit 1
    fi
else
    echo "  already built — skipping"
fi
echo "  juceaide: $JUCEAIDE_EXE"

# ── Step 2: Cross-compile SurgeXT standalone ─────────────────────────────────
echo ""
echo "=== Step 2: Cross-compile SurgeXT standalone for aarch64 ==="
echo "  configuring..."
cmake -S "$SCRIPT_DIR" -B "$CROSS_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DJUCE_JUCEAIDE_PATH="$JUCEAIDE_EXE" \
    -DSURGE_BUILD_FX=OFF \
    -DSURGE_BUILD_TESTRUNNER=OFF \
    -DSURGE_BUILD_CLAP=OFF \
    -DSURGE_BUILD_LV2=OFF \
    -DSURGE_SKIP_VST3=ON \
    -DSURGE_SKIP_LUA=TRUE \
    -DSURGE_STANDALONE_AUTO_OSC_IN=ON \
    -DSST_PLUGININFRA_FILESYSTEM_FORCE_PLATFORM=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja
echo "  building..."
cmake --build "$CROSS_BUILD" --target surge-xt_Standalone

# ── Step 3: Collect dist ──────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
echo ""
echo "=== Collecting ==="

src="$(find "$CROSS_BUILD" -type f -name "SurgeXT" ! -path "*/_deps/*" 2>/dev/null | head -1)"
if [[ -n "$src" ]]; then
    cp "$src" "$DIST_DIR/SurgeXT"
    echo "  -> dist/SurgeXT"
else
    echo "  WARNING: SurgeXT binary not found in $CROSS_BUILD"
fi

echo ""
echo "=== Done ==="
echo "  Run:  python deploy.py --target-ip <pi-ip>"
echo "  First time:  python deploy.py --target-ip <pi-ip> --with-data  (~490 MB)"
