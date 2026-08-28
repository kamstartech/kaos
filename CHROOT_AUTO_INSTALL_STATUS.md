# Kaos Distro Install Status

## Status: ✅ MULTI-DISTRO PIPELINE OPERATIONAL

Distros are installed on-demand via the Phosh app or directly through the root bridge.
Kaos Kali distro has been verified end-to-end on device (April 2026).

---

## Architecture

### Install Flow
```
Phosh App (SetupManager.java)
  → downloads rootfs to app cache
  → sends bridge command: echo 'kaos-chroot-install --distro <name> -c restore <archive> <path>' | nc 127.0.0.1 30000
  → kaos-chroot-install extracts, detects, renames to distros.conf path
  → namespace ready at /data/.stowaway/<distro>/
```

### Distro Registry
**File:** `kaos/scripts/distros.conf`  
**Device location:** `/data/.stowaway/distros.conf`

| Distro | Path | Status | Archive |
|--------|------|--------|---------|
| kali | `/data/.stowaway/kali` | installed | tar.xz |
| alpine | `/data/.stowaway/alpine-arm64` | available | tar.gz |
| debian | `/data/.stowaway/debian-arm64` | available | debootstrap |
| ubuntu | `/data/.stowaway/ubuntu-arm64` | available | tar.gz |
| arch | `/data/.stowaway/arch-arm64` | available | tar.gz |
| sailfish | `/data/.stowaway/sailfish` | installed | tar.bz2 |

---

## Phosh App: Installation Pipeline

**Source:** `kaos/apps/phosh-app/src/com/kaos/phosh/SetupManager.java`

Key configuration:
- Connect timeout: **30 seconds**
- Read timeout: **300 seconds** (for large rootfs downloads)
- Bridge command format: positional args (not quoted `-c "..."`)
- Stderr drain: parallel thread (prevents bridge deadlock)
- Post-install verification: checks `bin/sh` + `/etc` via bridge before marking installed

**Distro status detection** (`DistroManager.java`):
- `existsViaBridge()` checks for real rootfs markers (`bin/sh` or `usr/bin/env` + `etc/` dir)
- Empty directories from failed extractions correctly detected as NOT installed
- UI shows "DEPLOY SYSTEM" for uninstalled distros, "START" for installed ones

---

## Installer Script

**Source:** `kaos/scripts/kaos-chroot-install`  
**Device path:** `/system/bin/kaos-chroot-install`

Features:
- Extracts `.tar.xz`, `.tar.gz`, `.tar.bz2` archives via `busybox_nh`
- Searches for rootfs using distro-name-prefixed globs first (`kali-arm64`, etc.)
- Handles flat tarballs (Alpine-style: no wrapper dir) by relocating into distro subdir
- Sets up resolv.conf, hostname, essential directories

---

## Boot Services

On `sys.boot_completed=1` (via `init.kaos.rc`):
- `kaos-bridge`: Starts root bridge on port 30000
- `kaos-service`: Ready to start distro namespaces on demand

---

## Manual Install (without Phosh app)

```bash
# Download rootfs (example: Kali minimal)
adb shell "mkdir -p /data/local/nh_install"
adb push kali-nethunter-rootfs-minimal-arm64.tar.xz /data/local/nh_install/kali-rootfs.tar.xz

# Install via bridge
adb shell "echo '/system/bin/kaos-chroot-install --distro kali -c restore /data/local/nh_install/kali-rootfs.tar.xz /data/.stowaway/kali' | nc 127.0.0.1 30000"

# Clean up
adb shell "rm -rf /data/local/nh_install"

# Start namespace
adb shell "echo '/system/bin/kaos-service --distro kali start' | nc 127.0.0.1 30000"

# Login
adb shell "/system/bin/kaos-starter --distro kali exec /bin/bash"
```

---

**Last Updated**: April 2026  
**Verified on device**: ae712b83 (Xiaomi Mi Mix 3 "perseus")
