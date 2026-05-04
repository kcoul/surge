#!/usr/bin/env bash
# Downloads required model files and embeds the VAD model into a C source pair
# that gets compiled into the bridge binary.
#
# Run this once before building, and re-run if the VAD model version changes.
#
# Usage: prepare-bridge-resources.sh [--skip-whisper] [--skip-embed]
#   --skip-whisper   skip downloading the whisper.cpp CPU/GPU models
#   --skip-embed     skip regenerating the embedded VAD C source

set -euo pipefail

SKIP_WHISPER=0
SKIP_EMBED=0
for arg in "$@"; do
    case "$arg" in
        --skip-whisper) SKIP_WHISPER=1 ;;
        --skip-embed)   SKIP_EMBED=1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
MODELS_DIR="$REPO_ROOT/libs/whisper.cpp/models"
GENERATED_DIR="$REPO_ROOT/extras/SurgeMidiToOscBridge/Source/generated"
EMBED_SCRIPT="$SCRIPT_DIR/embed_binary_as_c.sh"

echo "=== Surge MIDI to OSC Bridge — resource preparation ==="
echo "Repo root : $REPO_ROOT"
echo "Models dir: $MODELS_DIR"
echo ""

# ---------------------------------------------------------------------------
# VAD model — always required; embedded into the binary at build time
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

if [ "$SKIP_EMBED" -eq 0 ]; then
    echo ""
    echo "--- Embedding VAD model into C source ---"
    bash "$EMBED_SCRIPT" \
        "$VAD_MODEL" \
        "$GENERATED_DIR/embedded_vad_model.h" \
        "$GENERATED_DIR/embedded_vad_model.cpp" \
        "ggml_silero_vad_model"
    echo "Rebuild the bridge to pick up the new embedding."
else
    echo "(embedding skipped)"
fi

# ---------------------------------------------------------------------------
# Whisper CPU/GPU models — optional; loaded from models/ next to the binary
# ---------------------------------------------------------------------------
if [ "$SKIP_WHISPER" -eq 0 ]; then
    echo ""
    echo "--- Whisper models (CPU/GPU backend) ---"
    for model in tiny base; do
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
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=== Done ==="
echo ""
echo "Embedded VAD:"
ls -lh "$GENERATED_DIR/embedded_vad_model.cpp" 2>/dev/null \
    && echo "  (rebuild the bridge to include it)" \
    || echo "  NOT generated — re-run without --skip-embed"

echo ""
echo "Whisper models available for deployment (copy to models/ next to binary):"
for model in tiny base; do
    FILE="$MODELS_DIR/ggml-${model}.bin"
    if [ -f "$FILE" ]; then
        echo "  $(ls -lh "$FILE" | awk '{print $5, $9}')"
    else
        echo "  ggml-${model}.bin  [not downloaded]"
    fi
done
