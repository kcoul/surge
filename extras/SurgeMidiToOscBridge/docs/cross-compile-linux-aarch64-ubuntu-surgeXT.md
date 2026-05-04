# Cross-Compilation: Surge XT Standalone for aarch64 Ubuntu

This guide extends the bridge cross-compile setup
([cross-compile-linux-aarch64-ubuntu.md](cross-compile-linux-aarch64-ubuntu.md))
to build the **Surge XT standalone** for an aarch64 Ubuntu target such as
Raspberry Pi 5. Complete that guide first — the cross-compiler, multiarch system
libraries, and host `juceaide` are all shared prerequisites.

---

## Additional arm64 library

SurgeXT's JUCE build requires OpenGL for its GUI. The toolchain file pre-fills the
path `/usr/lib/aarch64-linux-gnu/libGL.so`, so the arm64 OpenGL development package
must be installed via multiarch before configuring:

```bash
sudo apt install libgl-dev:arm64
```

> If further undeclared transitive dependencies surface at link time (similar to
> libusb with HailoRT), install them the same way: identify the missing `.so` name
> from the linker error, find the corresponding `*-dev:arm64` package with
> `apt search`, and install it.

---

## Building

The CMake presets for SurgeXT already exist alongside the bridge presets. The
same host `juceaide` binary is reused — no rebuild needed if you already ran the
bridge setup.

### Step 1: Configure

```bash
cmake --preset linux-aarch64-ubuntu-surge
```

Build directory: `build-linux-aarch64-ubuntu`

Notable flags set by the preset:
- `SURGE_BUILD_FX=OFF`, `SURGE_BUILD_TESTRUNNER=OFF`, `SURGE_BUILD_CLAP=OFF`,
  `SURGE_BUILD_LV2=OFF`, `SURGE_SKIP_VST3=ON`, `SURGE_SKIP_LUA=TRUE` — builds
  only the standalone, skipping all plugin formats
- `SURGE_STANDALONE_AUTO_OSC_IN=ON` — the standalone automatically opens its OSC
  input port on launch, so the MIDI-to-OSC bridge can connect without manual setup
- `SST_PLUGININFRA_FILESYSTEM_FORCE_PLATFORM=ON` — bypasses a `std::filesystem`
  capability probe that hangs during cross-compilation

### Step 2: Build

```bash
cmake --build --preset build-linux-aarch64-ubuntu-surge
```

This builds only the `surge-xt_Standalone` target. With 4 parallel jobs on a
modern machine expect 10–20 minutes for a clean build.

### Step 3: Verify

```bash
file build-linux-aarch64-ubuntu/src/surge-xt/surge-xt_artefacts/Release/Standalone/SurgeXT
```

Expected output:
```
... ELF 64-bit LSB executable, ARM aarch64 ...
```

---

## Deploying to the target

The factory data lives in `resources/data/` in the repo (≈490 MB). SurgeXT on
Linux checks several locations in strict priority order and **uses the first one
whose directory exists**, regardless of whether it actually contains data:

| Priority | Path | Notes |
|---|---|---|
| 1 | `SurgeXTData/` walked up from binary | Portable mode; folder name is specific to this mechanism |
| 2 | `~/.local/share/Surge XT/` | User install (note capital letters and space) |
| 3 | `~/.local/share/surge-xt/` | User install (lowercase) |
| 4 | `/usr/local/share/surge-xt/` | System install (CMAKE_INSTALL_PREFIX default) |
| 5 | `/usr/share/surge-xt/` | System install |

> **Priority gotcha:** If a higher-priority directory exists on the filesystem —
> even empty — SurgeXT stops searching there and never reaches the lower-priority
> paths. Creating `~/.local/share/Surge XT/` for any reason (e.g. a failed copy
> attempt) will silently shadow a correctly populated `/usr/share/surge-xt/`.
> Before debugging data-not-found problems, check whether any of the
> higher-priority paths exist as directories:
> ```bash
> ls ~/.local/share/ | grep -i surge
> ls /usr/local/share/ | grep -i surge
> ```
> If a stale empty directory is shadowing your data, remove it:
> ```bash
> rm -rf ~/.local/share/"Surge XT"
> rm -rf ~/.local/share/surge-xt
> ```

In all cases the data root must contain `patches_factory/` directly — not inside
any named subdirectory. The `SurgeXTData` name is **only** the folder name for
portable mode; it has no meaning for the other paths.

---

### Option A: System install (no constraints on binary location)

Install the factory data to `/usr/share/surge-xt/` on the Pi. The binary can
live anywhere.

```bash
# Stage the data in the home directory first (avoids scp permission issues)
rsync -av resources/data/ user@<pi-ip>:~/surge-data-staging/

# Then on the Pi, move it into place as root
ssh user@<pi-ip> "sudo mkdir -p /usr/share/surge-xt && \
                  sudo rsync -av ~/surge-data-staging/ /usr/share/surge-xt/ && \
                  rm -rf ~/surge-data-staging"
```

Copy the binary to somewhere on `$PATH` (or anywhere convenient):

```bash
scp build-linux-aarch64-ubuntu/src/surge-xt/surge-xt_artefacts/Release/Standalone/SurgeXT \
    user@<pi-ip>:~/
```

Verify the data root is correct on the Pi:

```bash
ls /usr/share/surge-xt/
# Should show: patches_factory  wavetables  skins  ...
```

---

### Option B: User install (no sudo required)

Install the factory data to `~/.local/share/Surge XT/` on the Pi. Note the
capital letters and the space — the directory name must match exactly.

```bash
ssh user@<pi-ip> 'mkdir -p ~/.local/share/"Surge XT"'
rsync -av resources/data/ user@<pi-ip>:~/.local/share/"Surge XT"/
```

Verify:

```bash
ssh user@<pi-ip> 'ls ~/.local/share/"Surge XT"/'
# Should show: patches_factory  wavetables  skins  ...
```

---

### Option C: Portable mode (self-contained, binary + data travel together)

Place the binary and a `SurgeXTData/` folder alongside each other. SurgeXT walks
up the directory tree from the binary and uses the first `SurgeXTData/` it finds.

```
~/surge/
  SurgeXT              ← binary
  SurgeXTData/
    patches_factory/   ← data contents go directly here
    wavetables/
    skins/
    ...
```

```bash
ssh user@<pi-ip> "mkdir -p ~/surge/SurgeXTData"

scp build-linux-aarch64-ubuntu/src/surge-xt/surge-xt_artefacts/Release/Standalone/SurgeXT \
    user@<pi-ip>:~/surge/

rsync -av resources/data/ user@<pi-ip>:~/surge/SurgeXTData/
```

> **Why rsync and not `scp -r`?** `scp -r resources/data` (no trailing slash on
> source) copies the `data` directory itself, creating
> `SurgeXTData/data/patches_factory/` — one level too deep. `rsync` with a
> trailing slash on the source unambiguously copies the contents only.

Verify:

```bash
ssh user@<pi-ip> "ls ~/surge/SurgeXTData/"
# Should show: patches_factory  wavetables  skins  ...
```

---

### User data (patches, saved state)

SurgeXT uses a separate writable path for user content. It also walks up from the
binary first looking for `SurgeXTUserData/`, then falls back to
`~/.local/share/Surge XT/`. For initial testing no action is needed — this
directory is created automatically on first run.

---

## Running on the target

```bash
ssh user@<pi-ip>
cd ~/surge
./SurgeXT
```

Surge XT will auto-enable its OSC input port on startup (configured at build
time via `SURGE_STANDALONE_AUTO_OSC_IN`). You can then connect the bridge
running on the same machine or on the host:

```
Bridge target IP:   <pi-ip>
Bridge OSC port:    53280  (Surge XT default)
```

---

## CMake preset reference

| Preset | What it does |
|---|---|
| `host-juceaide-linux` | Configure native juceaide (shared with bridge, build once) |
| `build-host-juceaide-linux` | Build native juceaide |
| `linux-aarch64-ubuntu-surge` | Configure Surge XT for aarch64 Ubuntu |
| `build-linux-aarch64-ubuntu-surge` | Build Surge XT standalone for aarch64 Ubuntu |
