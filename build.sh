#!/usr/bin/env bash
# Cross-compile SurgeXT Standalone for aarch64 Ubuntu/RPi OS (RPi5) from WSL2.
#
# Prerequisites: see extras/SurgeMidiToOscBridge/docs/cross-compile-linux-aarch64-ubuntu-surgeXT.md
# Note: if you already ran the bridge build.sh, juceaide is already built and
# this script will skip Step 1 automatically.
#
# Usage:
#   ./build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/build/dist"

# CMake presets must be invoked from the directory containing CMakePresets.json.
cd "$SCRIPT_DIR"

# ── Step 1: Native juceaide (skip if already built) ───────────────────────────
echo "=== Step 1: Native juceaide ==="
JUCEAIDE_EXE="$(find "$SCRIPT_DIR/libs/JUCE/build" -name "juceaide" -type f 2>/dev/null | head -1)"
if [[ -z "$JUCEAIDE_EXE" || ! -x "$JUCEAIDE_EXE" ]]; then
    cmake --preset host-juceaide-linux
    cmake --build --preset build-host-juceaide-linux
    JUCEAIDE_EXE="$(find "$SCRIPT_DIR/libs/JUCE/build" -name "juceaide" -type f 2>/dev/null | head -1)"
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
cmake --preset linux-aarch64-ubuntu-surge
cmake --build --preset build-linux-aarch64-ubuntu-surge

# ── Step 3: Collect dist ──────────────────────────────────────────────────────
mkdir -p "$DIST_DIR"
echo ""
echo "=== Collecting ==="

src="$(find "$SCRIPT_DIR/build-linux-aarch64-ubuntu" -type f -name "SurgeXT" ! -path "*/_deps/*" 2>/dev/null | head -1)"
if [[ -n "$src" ]]; then
    cp "$src" "$DIST_DIR/SurgeXT"
    echo "  → dist/SurgeXT"
else
    echo "  WARNING: SurgeXT binary not found in build-linux-aarch64-ubuntu"
fi

echo ""
echo "=== Done ==="
echo "  Run:  python deploy.py --target-ip <pi-ip>"
echo "  First time:  python deploy.py --target-ip <pi-ip> --with-data  (~490 MB)"
