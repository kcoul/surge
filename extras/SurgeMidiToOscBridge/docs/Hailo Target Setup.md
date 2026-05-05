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
