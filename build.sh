#!/usr/bin/env bash
# Cross-compile SurgeXT Standalone for aarch64 — Linux (Ubuntu/RPi OS) or QNX.
#
# Prerequisites:
#   Linux: see extras/SurgeMidiToOscBridge/docs/cross-compile-linux-aarch64-ubuntu-surgeXT.md
#   QNX:   QNX SDP installed, QNX_HOST and QNX_TARGET exported, qcc/q++ in PATH
#
# Tip: if you already ran extras/SurgeMidiToOscBridge/build.sh, Step 1 is
# skipped automatically — both scripts share the same native juceaide.
#
# Usage:
#   ./build.sh                   # Linux (default)
#   ./build.sh --target qnx      # QNX

set -euo pipefail

TARGET=linux
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) TARGET="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

if [[ "$TARGET" != "linux" && "$TARGET" != "qnx" ]]; then
    echo "ERROR: --target must be 'linux' or 'qnx'"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JUCE_DIR="$SCRIPT_DIR/libs/JUCE"

# Shared juceaide lives in the bridge's native build so both scripts stay in sync.
BRIDGE_NATIVE="$SCRIPT_DIR/extras/SurgeMidiToOscBridge/build/native"
NATIVE_BUILD="$BRIDGE_NATIVE"

BUILD_DIR="$SCRIPT_DIR/build"

if [[ "$TARGET" == "qnx" ]]; then
    if [[ -z "${QNX_HOST:-}" || -z "${QNX_TARGET:-}" ]]; then
        echo "ERROR: QNX_HOST and QNX_TARGET must be set (source your QNX SDP environment first)"
        exit 1
    fi
    TOOLCHAIN="$JUCE_DIR/extras/Build/CMake/QNXAarch64Toolchain.cmake"
    CROSS_BUILD="$BUILD_DIR/aarch64-qnx"
    DIST_DIR="$BUILD_DIR/dist-qnx"
else
    TOOLCHAIN="$SCRIPT_DIR/cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake"
    CROSS_BUILD="$BUILD_DIR/aarch64-linux"
    DIST_DIR="$BUILD_DIR/dist-linux"
fi

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
echo "=== Step 2: Cross-compile SurgeXT standalone for aarch64-$TARGET ==="
echo "  toolchain: $TOOLCHAIN"
echo "  build dir: $CROSS_BUILD"
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
cmake --build "$CROSS_BUILD" --target surge-xt_Standalone --parallel 4

# ── Step 3: Collect dist ──────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
echo ""
echo "=== Collecting ==="

src="$(find "$CROSS_BUILD" -type f -name "SurgeXT" ! -path "*/_deps/*" 2>/dev/null | head -1)"
if [[ -n "$src" ]]; then
    cp "$src" "$DIST_DIR/SurgeXT"
    echo "  -> $DIST_DIR/SurgeXT"
else
    echo "  WARNING: SurgeXT binary not found in $CROSS_BUILD"
fi

echo ""
echo "=== Done ==="
if [[ "$TARGET" == "qnx" ]]; then
    echo "  Run:  python deploy.py --target-ip root@<pi-ip> --dist $DIST_DIR"
    echo "  NOTE: QNX requires root@ prefix — plain IP will use your local username and fail"
else
    echo "  Run:  python deploy.py --target-ip <pi-ip> --dist $DIST_DIR"
fi
