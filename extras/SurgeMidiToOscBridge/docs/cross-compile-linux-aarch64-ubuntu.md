# Cross-Compilation: aarch64 Ubuntu from Linux or WSL2

This guide covers building `SurgeMidiToOscBridge` for an **aarch64 Ubuntu** target
(e.g. Raspberry Pi 5) from either:

- A native **x86\_64 Ubuntu** host
- **WSL2** running Ubuntu 24.04 on a Windows machine

**Both paths use identical commands.** WSL2 is the recommended approach for Windows
users because it provides a genuine Linux environment — the same cross-toolchain
packages, the same multiarch apt workflow, the same CMake presets — without the
complexity of setting up a Windows-native cross-compiler.

> **Why WSL2 instead of a native Windows cross-toolchain?**
> The GCC-based aarch64-linux-gnu cross-compiler is a first-class citizen of Ubuntu's
> apt ecosystem. On Windows that same compiler requires either a third-party binary
> distribution (Arm GNU Toolchain) plus a manually assembled sysroot, or MSYS2. WSL2
> collapses all of that back to one `apt install` command and identical CMake presets.

---

## Prerequisites

All steps below run inside the WSL2 Ubuntu shell (or a native Ubuntu terminal).
From Windows, open WSL2 with:

```
wsl -d Ubuntu
```

### WSL2: clone the repo into the WSL2 native filesystem

**Do not work from the `/mnt/c/...` path of your Windows clone.** CMake caches
store absolute paths, so a cache built from Windows (`C:\...` paths) is
incompatible with one built from WSL2. The presets share build directories by
name (e.g. `libs/JUCE/build`), so if you have already configured or built from
the Windows side, CMake will immediately complain about a cache mismatch when
you try to run any Linux preset against the same tree.

The clean solution is a separate clone inside the WSL2 native filesystem:

```bash
cd ~
git clone --recurse-submodules <remote-url> repos/surge
cd repos/surge
```

This clone is independent from the Windows one, gets its own build directories,
and runs at full native filesystem speed rather than over the WSL2/Windows
virtual filesystem bridge.

### 1. Cross-compiler

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

This installs the `aarch64-linux-gnu-gcc` / `g++` toolchain, which is what
`cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake` expects.

### 2. CMake and Ninja

```bash
sudo apt install cmake ninja-build
```

Already present in WSL2 Ubuntu 24.04 on the machine used to develop this guide
(cmake 3.28.3, ninja 1.11.1).

### 3. arm64 multiarch system libraries

JUCE requires ALSA, FreeType, FontConfig, and X11. Ubuntu's multiarch support lets
you install arm64 development packages alongside the x86\_64 ones.

> **Important — fix apt sources before updating.** On Ubuntu 24.04, adding a foreign
> architecture causes `apt update` to request arm64 package lists from every
> configured source, most of which only carry amd64 and will return 404 errors. You
> must annotate your existing sources and add a `ports.ubuntu.com` entry for arm64
> **between** the `dpkg --add-architecture` and `apt update` steps below.
> See **[multiarch-arm64-apt-sources.md](multiarch-arm64-apt-sources.md)** for the
> exact commands.

```bash
sudo dpkg --add-architecture arm64
# ← follow multiarch-arm64-apt-sources.md here before continuing
sudo apt update
sudo apt install \
    libasound2-dev:arm64 \
    libfreetype-dev:arm64 \
    libfontconfig1-dev:arm64 \
    libx11-dev:arm64 \
    libxext-dev:arm64 \
    libxrandr-dev:arm64 \
    libxinerama-dev:arm64 \
    libxcursor-dev:arm64
```

`libcurl4-openssl-dev` must be installed separately due to a multiarch packaging
bug in Ubuntu 24.04. The package ships `/usr/bin/curl-config` in both the amd64
and arm64 variants, and the two files differ, so dpkg refuses to unpack them
alongside each other normally. The `--force-overwrite` option tells dpkg to
overwrite the conflicting file rather than abort:

```bash
sudo apt install -o Dpkg::Options::="--force-overwrite" libcurl4-openssl-dev:arm64
```

This overwrites the host's `/usr/bin/curl-config` with the arm64 version. That
script is only used when building software that calls `curl-config` directly on
the host; CMake's `find_package(CURL)` does not use it during cross-compilation,
so this has no effect on the cross-build.

The arm64 libraries land in `/usr/lib/aarch64-linux-gnu/` alongside the x86\_64
libraries in `/usr/lib/x86_64-linux-gnu/`. The cross-compile toolchain file
references this multiarch layout directly.

### 4. HailoRT

Two packages are provided in `extras/SurgeMidiToOscBridge/Resources/`:

| File | Use |
|---|---|
| `hailort_5.3.0_amd64.deb` | Native amd64 runtime (WSL2 / native Ubuntu host) |
| `hailort_5.3.0_arm64.deb` | arm64 libraries for cross-linking |

#### 4a. Native amd64 — install or upgrade

This makes HailoRT available for native builds and for running the bridge in WSL2
(Hailo hardware access in WSL2 requires additional USB/PCIe passthrough setup; see
WSL2 notes below):

```bash
sudo dpkg -i extras/SurgeMidiToOscBridge/Resources/hailort_5.3.0_amd64.deb
sudo ln -sf /usr/lib/libhailort.so.5.3.0 /usr/lib/libhailort.so
```

If HailoRT 5.2 was previously installed this upgrades it to 5.3.

#### 4b. arm64 cross-compile sysroot

Extract the arm64 package into a dedicated directory. CMake's cross-toolchain file
points `CMAKE_FIND_ROOT_PATH` at this prefix so `find_package(HailoRT)` picks up
the arm64 CMake config rather than the host one:

```bash
sudo mkdir -p /opt/hailo-cross/arm64
sudo dpkg-deb -x \
    extras/SurgeMidiToOscBridge/Resources/hailort_5.3.0_arm64.deb \
    /opt/hailo-cross/arm64/
sudo ln -sf \
    /opt/hailo-cross/arm64/usr/lib/libhailort.so.5.3.0 \
    /opt/hailo-cross/arm64/usr/lib/libhailort.so
```

After extraction:

```
/opt/hailo-cross/arm64/
  usr/
    include/hailo/            ← headers (architecture-neutral)
    lib/
      libhailort.so.5.3.0     ← arm64 ELF shared library
      libhailort.so            ← symlink for the linker
      cmake/
        HailoRT/
          HailoRTConfig.cmake  ← self-relative; finds the arm64 lib automatically
```

The CMake config files inside the deb compute their prefix from their own location,
so they work correctly regardless of where you extract the package.

#### 4c. libusb — HailoRT's transitive dependency

`libhailort.so` depends on `libusb-1.0` for its USB device transport path (Hailo
hardware can connect over USB as well as PCIe/M.2). The HailoRT CMake config does
not declare this dependency explicitly, so the linker only reports it missing at
the final link step. Install the arm64 libusb development package via multiarch:

```bash
sudo apt install libusb-1.0-0-dev:arm64
```

This places the arm64 `libusb-1.0.so` and `libusb-1.0.so.0` into
`/usr/lib/aarch64-linux-gnu/`, which is already in the cross-linker's search path
via the toolchain file's `CMAKE_EXE_LINKER_FLAGS_INIT`.

> **Why a separate prefix, not a system install?**
> Both the amd64 and arm64 packages ship `libhailort.so.5.3.0` to `/usr/lib/`
> (not to an arch-specific subdirectory), so they would conflict if installed
> system-wide at the same time. The `/opt/hailo-cross/arm64` prefix avoids that
> conflict. The cross-toolchain file adds this prefix at the front of its package
> search list, so it is found before the host-arch copy.

---

## Building

### Step 1: Build the host juceaide

JUCE requires a host-architecture helper binary (`juceaide`) that generates some
source artefacts during the target configure. Build it once from the host:

```bash
cmake --preset host-juceaide-linux
cmake --build --preset build-host-juceaide-linux
```

This produces:
```
libs/JUCE/build/JUCE-host/extras/Build/juceaide/juceaide_artefacts/Release/juceaide
```

The `linux-aarch64-ubuntu-midi-osc-bridge` preset passes this path via
`JUCE_JUCEAIDE_PATH` automatically.

### Step 2: Configure the cross-build

```bash
cmake --preset linux-aarch64-ubuntu-midi-osc-bridge
```

Build directory: `extras/SurgeMidiToOscBridge/build-linux-aarch64-ubuntu`

### Step 3: Build

```bash
cmake --build --preset build-linux-aarch64-ubuntu-midi-osc-bridge
```

### Step 4: Verify the output

```bash
file extras/SurgeMidiToOscBridge/build-linux-aarch64-ubuntu/extras/SurgeMidiToOscBridge/SurgeMidiToOscBridge_artefacts/Release/SurgeMidiToOscBridge
```

> The binary is named `SurgeMidiToOscBridge` (no spaces) because the CMake
> `PRODUCT_NAME` was set to match the target name. On older builds or Windows,
> the binary was named `Surge MIDI To OSC Bridge` — if you see that name in your
> artefacts directory, you are working from a stale build tree. Delete the build
> directory and reconfigure.

Expected output:

```
... ELF 64-bit LSB executable, ARM aarch64 ...
```

---

## Deploying to the target

### Voice model files

The bridge's voice-command path requires three model files that are not committed
to the repository and must be downloaded separately. They are resolved at runtime
by looking for a `models/` directory next to the binary (portable layout), falling
back to the source-relative path baked in at build time for dev builds.

| File | Required for |
|---|---|
| `ggml-silero-v6.2.0.bin` | VAD (always required when voice is enabled) |
| `ggml-tiny.bin` | whisper.cpp CPU/GPU backend, tiny model |
| `ggml-base.bin` | whisper.cpp CPU/GPU backend, base model |

Download using whisper.cpp's helper script from the repo root:

```bash
bash libs/whisper.cpp/models/download-ggml-model.sh tiny
bash libs/whisper.cpp/models/download-ggml-model.sh base
# Silero VAD model — check libs/whisper.cpp/models/ for the download script or
# source for your whisper.cpp version
```

The downloaded files land in `libs/whisper.cpp/models/`.

### Copy binary and models to the Pi

Create a deployment directory on the target and copy both:

```bash
ssh <user>@<pi-ip> "mkdir -p ~/bridge/models"

scp extras/SurgeMidiToOscBridge/build-linux-aarch64-ubuntu/extras/SurgeMidiToOscBridge/SurgeMidiToOscBridge_artefacts/Release/SurgeMidiToOscBridge \
    <user>@<pi-ip>:~/bridge/

scp libs/whisper.cpp/models/ggml-silero-v6.2.0.bin \
    libs/whisper.cpp/models/ggml-tiny.bin \
    libs/whisper.cpp/models/ggml-base.bin \
    <user>@<pi-ip>:~/bridge/models/
```

The runtime layout the bridge expects:

```
~/bridge/
  SurgeMidiToOscBridge
  models/
    ggml-silero-v6.2.0.bin   ← always needed
    ggml-tiny.bin             ← needed for whisper.cpp CPU/GPU backend
    ggml-base.bin             ← needed for whisper.cpp CPU/GPU backend
```

On subsequent rebuilds only the binary needs to be recopied; the `models/`
directory does not change.

### HailoRT runtime on the target

The target must have HailoRT 5.3.0 runtime installed:

```bash
# On the target Pi:
sudo dpkg -i hailort_5.3.0_arm64.deb
```

---

## WSL2-specific notes

- **Use a separate clone inside WSL2.** The Windows clone and the WSL2 clone must
  be kept separate. CMake caches embed absolute paths, and the presets share build
  directory names, so running a Linux preset against a tree that was already
  configured from Windows will immediately fail with a cache mismatch. Clone the
  repo into the WSL2 native filesystem (e.g. `~/repos/surge`) and work exclusively
  from there for all Linux/WSL2 builds. See the prerequisites section above.
- **Hailo hardware in WSL2:** The Hailo M.2 module is a PCIe device. WSL2 does not
  expose PCIe to the Linux environment by default. The amd64 HailoRT runtime can be
  installed and the bridge can be built in WSL2, but running the voice-command path
  requires the binary to be on the target device (or a native Ubuntu machine with
  the Hailo module). USB passthrough via `usbipd-win` does not apply to PCIe.

---

## CMake preset reference

| Preset | What it does |
|---|---|
| `host-juceaide-linux` | Configure native juceaide (prerequisite) |
| `build-host-juceaide-linux` | Build native juceaide |
| `linux-aarch64-ubuntu-midi-osc-bridge` | Configure bridge for aarch64 Ubuntu |
| `build-linux-aarch64-ubuntu-midi-osc-bridge` | Build bridge for aarch64 Ubuntu |

---

## Toolchain and preset internals

- **Toolchain file:** `cmake/linux-aarch64-ubuntu-crosscompile-toolchain.cmake`
  - Selects `aarch64-linux-gnu-gcc` / `g++`
  - Pre-fills JUCE's library detection variables with arm64 paths
  - Sets `CMAKE_FIND_ROOT_PATH` to include `HAILO_CROSS_PREFIX`
    (`/opt/hailo-cross/arm64` by default) before the system paths, ensuring the
    arm64 HailoRT cmake config is found first
- **Common preset:** `linux-aarch64-ubuntu-midi-osc-bridge-common`
  (in `cmake/CMakePresets.common.json`)
  - Sets `SURGE_HOST_MIDI_OSC_BRIDGE_ONLY=ON` for a fast bridge-only configure
  - Passes the pre-built `juceaide` path
- **Concrete preset:** `linux-aarch64-ubuntu-midi-osc-bridge`
  (in `cmake/CMakePresets.linux.json`)
  - Active only on Linux hosts (including WSL2, which reports as Linux)
  - Binary directory: `extras/SurgeMidiToOscBridge/build-linux-aarch64-ubuntu`
