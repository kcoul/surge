# Hailo NPU Target Setup (Raspberry Pi 5, aarch64 Ubuntu)

This guide covers setting up the Hailo NPU on the target device. It assumes:
- aarch64 Ubuntu Linux on a Raspberry Pi 5
- Hailo NPU installed on an NVMe adapter and detected by `lspci`

## 1. Install Linux Kernel Headers

The PCIe driver requires kernel headers matching the running kernel:

```bash
sudo apt install linux-headers-$(uname -r)
```

## 2. Install the PCIe Driver

```bash
sudo dpkg -i ./hailort-pcie-driver_5.3.0_all.deb
```

## 3. Install HailoRT

```bash
sudo dpkg -i ./hailort_5.3.0_arm64.deb
```

## 4. Verify Installation

Check that the driver loaded correctly:

```bash
sudo dmesg | grep -i hailo
```

Confirm the device is accessible:

```bash
hailortcli fw-control identify
```

or

```bash
hailortcli scan
```

If these commands return device information without errors, the NPU is ready for use on the target.

## 5. Deploy Model Files

Run `prepare-bridge-resources.sh` on the host first to ensure all models are downloaded.
Then SCP the full model zoo to the target. All commands run from the repo root on the host.

Create the target directories:

```bash
ssh <user>@<target> 'mkdir -p ~/bridge/models/hailo10h'
```

Whisper.cpp models (CPU / GPU backend):

```bash
scp libs/whisper.cpp/models/ggml-tiny.bin \
    libs/whisper.cpp/models/ggml-base.bin \
    libs/whisper.cpp/models/ggml-small.bin \
    <user>@<target>:~/bridge/models/
```

Hailo HEF models (NPU backend):

```bash
scp extras/SurgeMidiToOscBridge/Resources/models/hailo10h/Whisper-Tiny.hef \
    extras/SurgeMidiToOscBridge/Resources/models/hailo10h/Whisper-Base.hef \
    extras/SurgeMidiToOscBridge/Resources/models/hailo10h/Whisper-Small.hef \
    <user>@<target>:~/bridge/models/hailo10h/
```

Expected layout on the target:

```
~/bridge/
    models/
        ggml-tiny.bin
        ggml-base.bin
        ggml-small.bin
        hailo10h/
            Whisper-Tiny.hef
            Whisper-Base.hef
            Whisper-Small.hef
```

The Bridge binary searches `models/` next to itself before falling back to the source
tree, so as long as the binary also lives in `~/bridge/` the above layout requires no
further configuration.
