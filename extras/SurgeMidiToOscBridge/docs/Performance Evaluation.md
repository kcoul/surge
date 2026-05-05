# Performance Evaluation — Whisper Inference Backends

## Overview

SurgeMidiToOscBridge supports five inference configurations across two deployment
contexts. The goal of this evaluation is to characterise the word error rate (WER)
and real-time performance of each combination across three Whisper model sizes, to
inform which configuration is viable for live music performance use.

## Configurations Under Test

| # | Label        | Host / Target | Compute     | Notes |
|---|--------------|---------------|-------------|-------|
| 1 | Host CPU     | Host (x86_64) | CPU         | whisper.cpp, no GPU offload |
| 2 | Host GPU     | Host (x86_64) | GPU (CUDA)  | whisper.cpp with CUDA backend |
| 3 | Host NPU     | Host (x86_64) | Hailo-10H   | HailoRT genai Speech2Text, M.2 slot |
| 4 | Target CPU   | Target (aarch64) | CPU      | whisper.cpp on Raspberry Pi 5 SoC |
| 5 | Target NPU   | Target (aarch64) | Hailo-10H | HailoRT genai Speech2Text, NVMe adapter |

**Host**: x86_64 Ubuntu 24.04 desktop/laptop with Hailo-10H in M.2 slot  
**Target**: Raspberry Pi 5, aarch64 Ubuntu, Hailo-10H on NVMe adapter (detected via `lspci`)

## Model Sizes

| Model | whisper.cpp file    | Hailo HEF          | Approx. size | Expected WER |
|-------|---------------------|--------------------|--------------|--------------|
| Tiny  | `ggml-tiny.bin`     | `Whisper-Tiny.hef` | ~39 MB       | Higher       |
| Base  | `ggml-base.bin`     | `Whisper-Base.hef` | ~142 MB      | Moderate     |
| Small | `ggml-small.bin`    | `Whisper-Small.hef`| ~466 MB      | Lower        |

> **Note**: Small may exceed practical latency limits on Target CPU given the
> Raspberry Pi 5's single-core throughput. Target NPU viability for Small is
> also to be confirmed — it may run but with higher HEF load time.

## Metrics to Capture

- **RTF** (Real-Time Factor): inference time / audio duration. RTF < 1.0 required for live use.
- **WER** (Word Error Rate): measured against a fixed reference transcript.
- **First-token latency**: time from end of utterance to first transcribed word appearing.
- **HEF / model load time**: one-off startup cost, relevant for live rig setup.

## Results Matrix (to be filled)

### RTF

|       | Tiny | Base | Small |
|-------|------|------|-------|
| Host CPU  | | | |
| Host GPU  | | | |
| Host NPU  | | | |
| Target CPU | | | |
| Target NPU | | | |

### WER (%)

|       | Tiny | Base | Small |
|-------|------|------|-------|
| Host CPU  | | | |
| Host GPU  | | | |
| Host NPU  | | | |
| Target CPU | | | |
| Target NPU | | | |

### First-token latency (ms)

|       | Tiny | Base | Small |
|-------|------|------|-------|
| Host CPU  | | | |
| Host GPU  | | | |
| Host NPU  | | | |
| Target CPU | | | |
| Target NPU | | | |

## Hypotheses

- Host NPU should match or exceed Host GPU RTF while consuming less host CPU/power.
- Target NPU should substantially outperform Target CPU, potentially matching Host CPU.
- Small on Target CPU is expected to be too slow for live use (RTF > 1.0).
- Small on Target NPU is unknown — could be the most interesting data point.
- WER should be consistent across backends for the same model size (same weights, different execution engine).

## Test Methodology

- Fixed audio stimulus: a known spoken phrase or short monologue, recorded or played back
  identically for each run to eliminate acoustic variability.
- Measure over N repeated runs, report median RTF and WER.
- Backends selected via the UI combo boxes; no code changes between runs.
- Log output (`appendTranscriptLine`) timestamps used for latency measurement.

## Presentation Context

Results will be presented at **Audio Dev Con** as a 5-way shootout. The narrative arc:

1. Establish baseline: Host CPU (familiar territory for most devs).
2. Show GPU offload benefit on host.
3. Show NPU: same or better RTF, lower host resource impact — frees CPU for audio DSP.
4. Move to target: CPU is the constraint, RTF likely > 1 for Base/Small.
5. NPU on target unlocks what CPU cannot — the key result justifying the hardware investment.
