# Fixing apt 404 Errors After Adding the arm64 Architecture

## Why this happens

When you run `sudo dpkg --add-architecture arm64`, apt starts trying to download
package lists for arm64 from **every configured source** — including ones that only
carry amd64 packages. Most sources fail with 404 because they simply don't host
arm64:

- `archive.ubuntu.com/ubuntu` — serves amd64 and i386 only
- `security.ubuntu.com/ubuntu` — same
- Launchpad PPAs — amd64 and i386 only
- Third-party repos (e.g. Beyond Compare) — amd64 only

Ubuntu's arm64 packages live on a separate mirror: `ports.ubuntu.com/ubuntu-ports`.

The fix is two-part:

1. **Annotate every existing source** with the architecture(s) it actually serves, so
   apt stops requesting arm64 from amd64-only mirrors.
2. **Add a new source** pointing at `ports.ubuntu.com/ubuntu-ports` for arm64.

---

## Ubuntu 24.04 uses DEB822 format

Ubuntu 24.04 stores its sources in `/etc/apt/sources.list.d/ubuntu.sources` using
the newer **DEB822** format (`.sources` files) rather than the classic one-line
format (`.list` files). Both formats are in use simultaneously — Ubuntu's own sources
use DEB822, third-party repos often still use `.list`.

Each format has its own syntax for architecture annotations.

---

## Before you start — identify which files need attention

Run these two commands to see exactly which files on your system are missing
architecture annotations:

```bash
# DEB822 .sources files without an Architectures: field
grep -L "Architectures:" /etc/apt/sources.list.d/*.sources

# Old-format .list files with unannotated deb lines
grep -v "arch=" /etc/apt/sources.list.d/*.list | grep "^deb "
```

Any file or line returned by these commands needs to be edited before `apt update`
will succeed cleanly.

---

## Step 1 — Annotate the Ubuntu main sources (DEB822)

> **Upgraded from Ubuntu 22.04 or earlier?** Ubuntu does not automatically migrate
> your sources to DEB822 format when upgrading between releases. If your system was
> upgraded rather than freshly installed, check whether the old file still has active
> content:
> ```bash
> grep -v "^#" /etc/apt/sources.list | grep "^deb "
> ```
> If that command prints any lines, your main sources are still in the old one-line
> format in `/etc/apt/sources.list` rather than in
> `/etc/apt/sources.list.d/ubuntu.sources`. In that case, **skip this step** and
> instead follow **Step 4** to annotate those lines with `[arch=amd64]`, treating
> `/etc/apt/sources.list` exactly like any other `.list` file.

Open the file in nano:

```bash
sudo nano /etc/apt/sources.list.d/ubuntu.sources
```

The file contains two stanzas. Add `Architectures: amd64` to **each stanza**,
immediately after the `Components:` line. The result should look like this:

```
Types: deb
URIs: http://archive.ubuntu.com/ubuntu/
Suites: noble noble-updates noble-backports
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

## Ubuntu security updates.
Types: deb
URIs: http://security.ubuntu.com/ubuntu/
Suites: noble-security
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
```

Save and exit: `Ctrl+O`, `Enter`, `Ctrl+X`.

---

## Step 2 — Create the arm64 ports source (DEB822)

Ubuntu's arm64 packages are served from `ports.ubuntu.com/ubuntu-ports`, not from
`archive.ubuntu.com`. Create a new file for this:

```bash
sudo nano /etc/apt/sources.list.d/ubuntu-arm64-ports.sources
```

Type the following content in full:

```
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-backports
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble-security
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
```

Save and exit: `Ctrl+O`, `Enter`, `Ctrl+X`.

> **Suite naming:** `ports.ubuntu.com` uses the same suite names as the main mirror
> (`noble`, `noble-updates`, `noble-security`, etc.), so this file mirrors the
> structure of `ubuntu.sources` exactly, just with a different URI and
> `Architectures: arm64`.

---

## Step 3 — Annotate third-party DEB822 sources

Any `.sources` file that lacks an `Architectures:` line will cause the same 404
problem. Add `Architectures: amd64` to each one, after the `Components:` line.

**Launchpad PPAs** (e.g. deadsnakes):

```bash
sudo nano /etc/apt/sources.list.d/deadsnakes-ubuntu-ppa-noble.sources
```

Find the `Components:` line and add `Architectures: amd64` on the line immediately
below it:

```
...
Components: main
Architectures: amd64
Signed-By:
...
```

Save and exit: `Ctrl+O`, `Enter`, `Ctrl+X`.

**General rule:** PPAs and most third-party `.sources` files never carry arm64
packages. Always add `Architectures: amd64` to them.

---

## Step 4 — Annotate third-party one-line sources

For old-format `.list` files, architecture goes inside the `[...]` qualifier at the
start of each `deb` line.

Open the file in nano:

```bash
sudo nano /etc/apt/sources.list.d/scootersoftware.list
```

Find the `deb` line. It will look something like:

```
deb [signed-by=/usr/share/keyrings/scootersoftware-keyring.gpg] https://www.scootersoftware.com/ bcompare4 non-free
```

Add `arch=amd64 ` at the start of the `[...]` block (note the space after `amd64`):

```
deb [arch=amd64 signed-by=/usr/share/keyrings/scootersoftware-keyring.gpg] https://www.scootersoftware.com/ bcompare4 non-free
```

Save and exit: `Ctrl+O`, `Enter`, `Ctrl+X`.

If a `.list` file has no `[...]` qualifier at all, add one:

```
# Before:
deb https://example.com/repo stable main

# After:
deb [arch=amd64] https://example.com/repo stable main
```

---

## Step 5 — Verify

```bash
sudo apt update
```

A successful run will show lines fetching from both mirrors:

```
Get:1 http://ports.ubuntu.com/ubuntu-ports noble InRelease [...]
Get:2 http://archive.ubuntu.com/ubuntu noble InRelease [...]
...
```

There should be no 404 errors. If any remain, check the output to see which
source is still causing them, then open that file in nano and add the missing
architecture annotation.

---

## General principle for future sources

When adding any new apt source on a multiarch system:

- **DEB822 `.sources` file:** Always include an `Architectures:` field. Use
  `Architectures: amd64` for native-only sources, or `Architectures: amd64 arm64`
  if the source genuinely provides both.
- **Old-format `.list` file:** Always include `[arch=amd64]` (or the appropriate
  set) in the qualifier bracket of every `deb` line.

This applies to native Ubuntu hosts and WSL2 equally.
