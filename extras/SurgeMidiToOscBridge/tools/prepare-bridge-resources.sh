#!/usr/bin/env bash
# Downloads model files required by the bridge.
#
# The silero VAD model is embedded into the binary at build time by CMake
# (via tools/embed_binary_as_c.sh). This script only needs to ensure the
# model file is present before you configure; CMake's add_custom_command
# handles embedding and only re-runs when the model file changes.
#
# Run this once before the first build, and re-run if you want to update
# to a newer model version.
#
# Usage: prepare-bridge-resources.sh [--skip-whisper]
#   --skip-whisper   skip downloading the whisper.cpp CPU/GPU models

set -euo pipefail

SKIP_WHISPER=0
for arg in "$@"; do
    case "$arg" in
        --skip-whisper) SKIP_WHISPER=1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
MODELS_DIR="$REPO_ROOT/libs/whisper.cpp/models"

echo "=== Surge MIDI to OSC Bridge — resource preparation ==="
echo "Repo root : $REPO_ROOT"
echo "Models dir: $MODELS_DIR"
echo ""

# ---------------------------------------------------------------------------
# VAD model — embedded into the binary by CMake at build time
# ---------------------------------------------------------------------------
echo "--- Silero VAD model ---"
VAD_VERSION="silero-v6.2.0"
VAD_MODEL="$MODELS_DIR/ggml-${VAD_VERSION}.bin"

if [ ! -f "$VAD_MODEL" ]; then
    echo "Downloading ${VAD_VERSION}..."
    bash "$MODELS_DIR/download-vad-model.sh" "$VAD_VERSION" "$MODELS_DIR"
else
    echo "Already present: $VAD_MODEL"
fi
echo "CMake will embed this into the binary automatically at build time."

# ---------------------------------------------------------------------------
# Whisper CPU/GPU models — optional; loaded from models/ next to the binary
# ---------------------------------------------------------------------------
if [ "$SKIP_WHISPER" -eq 0 ]; then
    echo ""
    echo "--- Whisper models (CPU/GPU backend) ---"
    for model in tiny base small; do
        MODEL_FILE="$MODELS_DIR/ggml-${model}.bin"
        if [ ! -f "$MODEL_FILE" ]; then
            echo "Downloading ggml-${model}..."
            bash "$MODELS_DIR/download-ggml-model.sh" "$model" "$MODELS_DIR"
        else
            echo "Already present: $MODEL_FILE"
        fi
    done
else
    echo ""
    echo "--- Whisper models skipped ---"
fi

# ---------------------------------------------------------------------------
# Hailo HEF models — for NPU inference on hailo10h
# ---------------------------------------------------------------------------
echo ""
echo "--- Hailo HEF models (Hailo-10H NPU backend) ---"
HAILO_MODELS_DIR="$REPO_ROOT/extras/SurgeMidiToOscBridge/Resources/models/hailo10h"
mkdir -p "$HAILO_MODELS_DIR"
HAILO_HEF_BASE_URL="https://dev-public.hailo.ai/v5.3.0/blob"

for model in Tiny Base Small; do
    HEF_FILE="$HAILO_MODELS_DIR/Whisper-${model}.hef"
    if [ ! -f "$HEF_FILE" ]; then
        echo "Downloading Whisper-${model}.hef..."
        curl -fL "${HAILO_HEF_BASE_URL}/Whisper-${model}.hef" -o "$HEF_FILE"
    else
        echo "Already present: $HEF_FILE"
    fi
done

echo ""
echo "Deploy HEFs to target:"
echo "  scp $HAILO_MODELS_DIR/*.hef <user>@<target>:~/bridge/models/hailo10h/"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=== Done ==="
echo ""
echo "VAD model (embedded by CMake at build time):"
ls -lh "$VAD_MODEL" 2>/dev/null | awk '{print "  " $5, $9}' \
    || echo "  NOT present — download failed?"
echo "  Reconfigure to trigger embedding: cmake --preset linux-aarch64-ubuntu-midi-osc-bridge"

echo ""
echo "Whisper CPU/GPU models available for deployment (copy to models/ next to binary):"
for model in tiny base small; do
    FILE="$MODELS_DIR/ggml-${model}.bin"
    if [ -f "$FILE" ]; then
        echo "  $(ls -lh "$FILE" | awk '{print $5, $9}')"
    else
        echo "  ggml-${model}.bin  [not downloaded]"
    fi
done

echo ""
echo "Hailo HEF models available for deployment (scp to ~/bridge/models/hailo10h/ on target):"
for model in Tiny Base Small; do
    FILE="$HAILO_MODELS_DIR/Whisper-${model}.hef"
    if [ -f "$FILE" ]; then
        echo "  $(ls -lh "$FILE" | awk '{print $5, $9}')"
    else
        echo "  Whisper-${model}.hef  [not downloaded]"
    fi
done
