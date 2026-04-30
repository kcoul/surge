# Surge OSC Probe

Small UDP/OSC receiver for checking whether OSC packets reach a Linux target.
It has no JUCE dependency and is intended for quick native or cross builds.

## Native build

```sh
cmake -S extras/SurgeOscProbe -B build-osc-probe
cmake --build build-osc-probe
./build-osc-probe/surge-osc-probe 53280
```

## Example aarch64 Ubuntu cross build

```sh
cmake -S extras/SurgeOscProbe -B build-osc-probe-aarch64 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build-osc-probe-aarch64
```

Copy `build-osc-probe-aarch64/surge-osc-probe` to the target and run:

```sh
./surge-osc-probe 53280
```

The probe prints the sender IP/port, packet size, decoded OSC address and common
float/int/string arguments, plus a short hex dump.
