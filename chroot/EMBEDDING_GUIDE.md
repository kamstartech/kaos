# Embedding Kali Chroot in ROM Build

This guide explains how to embed the Kali Linux chroot into the Perseus ROM for automatic installation.

## Overview

The Kali chroot can be:
1. **Downloaded by user** via NetHunter app (recommended)
2. **Embedded in ROM** for automatic installation at first boot (advanced)

## Option 1: User Downloads via App (Recommended)

**Advantages:**
- Smaller ROM size
- User chooses variant (nano/minimal/full)
- Easier ROM updates
- More flexible

**User Instructions:**
1. Flash ROM
2. Install NetHunter app from: `kaos/apps/`
3. Open app → Kali Chroot Manager
4. Download desired variant

## Option 2: Embed in ROM (Advanced)

**Advantages:**
- Chroot available immediately after flash
- No additional downloads needed
- Fully offline installation

**Disadvantages:**
- Larger ROM size (+50MB to +1.5GB)
- Fixed variant choice
- Longer build time

### Step-by-Step: Embed Chroot

#### 1. Download Kali Chroot

The official Kali NetHunter chroots are available through the NetHunter app download mechanism or can be built from source.

**For pre-built chroots, check:**
- NetHunter GitLab releases
- Offensive Security downloads
- Community mirrors

**Variants:**
```bash
# Nano (~50MB)
kalifs-arm64-nano.tar.xz

# Minimal (~140MB) - Recommended
kalifs-arm64-minimal.tar.xz

# Full (~1.5GB)
kalifs-arm64-full.tar.xz
```

#### 2. Place Chroot in Build

```bash
# Copy to chroot directory
cp kalifs-arm64-minimal.tar.xz \
   kaos/chroot/kalifs.tar.xz
```

#### 3. Update Build Configuration

Edit `kaos/kaos.mk`:

```makefile
# Add chroot to ROM (optional - increases ROM size)
# Uncomment to embed chroot in ROM
# PRODUCT_COPY_FILES += \
#     kaos/chroot/kalifs.tar.xz:$(TARGET_COPY_OUT_SYSTEM)/etc/nethunter/kalifs.tar.xz
```

#### 4. Add Init Service

Create `kaos/init.chroot.rc`:

```
# Kaos Chroot Installation Service
service kaos-chroot-install /system/bin/kaos-chroot-install
    class late_start
    user root
    group root
    oneshot
    disabled

on property:sys.boot_completed=1
    start kaos-chroot-install
```

Add to `kaos.mk`:
```makefile
PRODUCT_COPY_FILES += \
    kaos/init.chroot.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.chroot.rc
```

#### 5. Build ROM

```bash
source build/envsetup.sh
lunch lineage_perseus-userdebug
mka bacon
```

**Build Impact:**
- Nano: +50MB ROM size, +5min build time
- Minimal: +140MB ROM size, +10min build time
- Full: +1.5GB ROM size, +30min build time

### Verification After Flash

```bash
# Check if chroot is embedded
adb shell ls -lh /system/etc/kaos/kalifs.tar.xz

# Check installation status
adb shell su 0 /system/bin/kaos-starter status

# View installation log
adb shell cat /data/local/tmp/nh_chroot_install.log

# Enter chroot (if installed)
adb shell su 0 /system/bin/kaos-starter shell
```

## Building Chroot from Source (Alternative)

Instead of downloading, you can build the chroot yourself:

```bash
# Clone Kaos build scripts
git clone https://gitlab.com/kalilinux/build-scripts/kali-nethunter-project.git
cd kali-nethunter-project

# Build chroot (requires Debian/Kali host)
./build.py --chroot minimal --arch arm64

# Output will be in output/
cp output/kalifs-arm64-minimal.tar.xz \
   /path/to/android/kaos/chroot/kalifs.tar.xz
```

## Current Implementation

✅ Chroot management scripts installed (`kaos-starter`, `kaos-chroot-install`)
✅ Scripts integrated in build system
✅ Documentation provided
⏸️  Chroot embedding disabled by default (user downloads via app)

To enable chroot embedding, follow Option 2 above.

## Recommendations

**For most users:** Use Option 1 (app download)
- Keeps ROM size reasonable
- Flexible variant selection
- Easy to update chroot independently

**For offline/enterprise deployments:** Use Option 2 (embed)
- No internet required
- Consistent deployment
- Fully automated setup

## Troubleshooting

**Chroot not installing:**
```bash
# Check if archive exists
adb shell ls -l /system/etc/kaos/kalifs.tar.xz

# Check installation logs
adb shell cat /data/local/tmp/nh_chroot_install.log

# Manually trigger installation
adb shell su 0 /system/bin/kaos-chroot-install
```

**Chroot mount fails:**
```bash
# Check permissions
adb shell su 0 ls -ld /data/local/nhsystem/kali-arm64

# Manually mount
adb shell su 0 /system/bin/kaos-starter start
```

**Out of space:**
- Nano variant requires ~150MB free space
- Minimal requires ~500MB
- Full requires ~5GB
- Use smaller variant or free up storage

## References

- NetHunter Documentation: https://www.kali.org/docs/nethunter/
- Build Scripts: https://gitlab.com/kalilinux/build-scripts/kali-nethunter-project
- NetHunter App: https://store.nethunter.com/
