# Kaos Chroot Mount Architecture

## Overview

The Kali chroot uses a two-directory architecture to work around Android's `nosuid` restriction on `/data`.

## The Problem

Android mounts `/data` with `nosuid` flag, which prevents setuid binaries (like `sudo`) from working:

```
/data/local/nhsystem/kali-arm64/usr/bin/sudo: effective uid is not 0
```

## The Solution

**Two directories:**

| Directory | Purpose |
|-----------|---------|
| `/data/local/nhsystem/kali-arm64` | Raw extracted chroot files (from tarball) |
| `/data/local/nhsystem/kalifs` | Mount point with SUID enabled |

**Mount flow** (in `kaos-service mounts`):
1. Bind mount `kali-arm64` → `kalifs`
2. Remount `kalifs` with `suid` option enabled
3. Mount `/proc`, `/sys`, `/dev`, `/dev/pts` into `kalifs`
4. Mount `/system`, `/vendor`, `/sdcard` for Android integration

## Why This Works

```bash
# Step 1: Bind mount
mount -o bind /data/local/nhsystem/kali-arm64 /data/local/nhsystem/kalifs

# Step 2: Enable SUID
mount -o remount,suid /data/local/nhsystem/kalifs
```

Now `sudo` and other setuid binaries work inside the chroot.

## Source Code Verification

From `scripts/kaos-service mounts` (lines 6-7, 29-36):

```bash
REAL_CHROOT="/data/local/nhsystem/kali-arm64"
CHROOT="/data/local/nhsystem/kalifs"
...
# Bind mount and enable SUID
mount -o bind "$REAL_CHROOT" "$CHROOT"
mount -o remount,suid "$CHROOT"
```

Wired in `init.kaos.rc` (line 64):

```
service kaos-mounts /system/bin/kaos-service mounts
```

## Script Responsibilities

| Script | Uses | Purpose |
|--------|------|---------|
| `kaos-chroot-install` | `kali-arm64` | Extracts tarball here |
| `kaos-service mounts` | Both | Mounts kali-arm64 → kalifs with suid |
| `kaos-service` | `kalifs` | Creates PID namespace with fake PID 1 |
| `kaos-starter` | `kalifs` | Enters the mounted chroot |
| `kaos-ssh-server` | `kalifs` | Runs SSH in mounted chroot |

## Boot Sequence

1. `kaos-chroot-install` extracts to `kali-arm64` (first boot only)
2. `kaos-mounts` runs:
   - Creates `kalifs` mount point
   - Bind mounts `kali-arm64` → `kalifs`
   - Remounts with `suid` enabled
   - Mounts `/dev`, `/proc`, `/sys`, etc.
3. `kaos-namespace` starts `kaos-service` (creates fake PID 1)
4. `kaos-ssh` starts SSH server in `kalifs`
5. User can run `kaos-starter` to enter `kalifs`

## Mounted Filesystems

Inside `/data/local/nhsystem/kalifs`:

| Mount | Type | Purpose |
|-------|------|---------|
| `/proc` | proc | Process info |
| `/sys` | sysfs | Kernel info |
| `/dev` | bind | Device nodes |
| `/dev/pts` | bind | Terminal devices |
| `/system` | bind (ro) | Android system (for libhybris) |
| `/vendor` | bind (ro) | Vendor libs (for libhybris) |
| `/sdcard` | bind | User storage access |

## Verification

```bash
# Check mounts
cat /proc/mounts | grep kalifs

# Should show suid option:
# /data/local/nhsystem/kali-arm64 /data/local/nhsystem/kalifs ... suid ...

# Test sudo works
kaos-starter
sudo whoami
# Output: root
```

## Logs

Mount operations logged to: `/data/adb/nh_mounts.log`

---

**Last Updated**: 2026-01-25
