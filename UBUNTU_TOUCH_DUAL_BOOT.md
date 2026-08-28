# Ubuntu Touch Dual Boot with Kexec

> **⚠️ SUPERSEDED — 2026-07-28**  
> The kexec approach documented here is no longer maintained. Ubuntu Touch is now booted via **`type=halium` switch_root** in the MultiROM trampoline, which does not require a separate boot.img or kexec. See `~/hadk/docs/superpowers/plans/2026-07-28-halium-boot-mode-activation.md` for the current implementation.

**Date**: 2024-01-05  
**Device**: Perseus (Xiaomi Mi MIX 3)  
**Base ROM**: LineageOS 21 (Android 14) + Kaos  
**Dual Boot**: Android + Ubuntu Touch ~~via kexec~~ *(legacy — see banner)*

## Overview

This implementation ~~allows~~ **allowed** booting Ubuntu Touch alongside Android without modifying partitions or the Android boot image. The Ubuntu Touch boot.img was **automatically built** during ROM compilation and loaded via kexec when needed.

## Architecture

### File Structure

```
Build artifacts:
out/target/product/perseus/data/boot_images/ubuntu/
├── boot-ubuntu.img          ← Ubuntu Touch boot image (auto-built)
├── kernel                   ← Extracted kernel (auto-extracted)
└── initrd.img              ← Extracted initramfs (auto-extracted)

Device after flash:
/data/boot_images/ubuntu/
├── boot-ubuntu.img          ← Ubuntu Touch boot image (full image, optional)
├── kernel                   ← Extracted kernel (for kexec)
└── initrd.img              ← Extracted initramfs (for kexec)

/vendor/bin/
└── nh-ubuntu               ← Helper script for Ubuntu boot
```

### Boot Flow

```
┌─────────────────────────────────────────────────────────────┐
│  Normal Boot: Android                                        │
├─────────────────────────────────────────────────────────────┤
│  Bootloader → Android boot.img → LineageOS + Kaos     │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Dual Boot: Android → Ubuntu Touch                          │
├─────────────────────────────────────────────────────────────┤
│  1. Boot Android normally                                    │
│  2. User runs: nh-ubuntu boot                               │
│  3. Script extracts kernel/initrd from ubuntu-boot.img      │
│  4. kexec loads Ubuntu kernel                               │
│  5. Device reboots into Ubuntu Touch                        │
│  6. To return: reboot (loads Android boot.img)              │
└─────────────────────────────────────────────────────────────┘
```

## Implementation Details

### 1. Boot Image Placement

**Build Time:**
- Ubuntu boot.img **automatically built** from:
  - Kernel: Same unified kernel as Android
  - Ramdisk: Halium initramfs from `device/xiaomi/perseus/ubuntu/ramdisk/halium.cpio.gz`
  - Cmdline: Ubuntu-specific with AppArmor support
- Output: `out/target/product/perseus/data/boot_images/ubuntu/boot-ubuntu.img`
- Kernel and initrd extracted during build to same directory
- All files packaged together in /data partition

**Runtime:**
- boot.img, kernel, and initrd all in `/data/boot_images/ubuntu/`
- Pre-extracted, ready for instant kexec loading
- No runtime extraction needed

### 2. Helper Script: nh-ubuntu

Located at `/vendor/bin/nh-ubuntu`, provides:

```bash
# Check status
nh-ubuntu status

# Load kernel into memory (from pre-extracted files)
nh-ubuntu load

# Execute loaded kernel (reboot to Ubuntu)
nh-ubuntu exec

# Load and execute in one step
nh-ubuntu boot
```

No extraction commands needed - files are pre-extracted during build.

### 3. Kexec Loading

```bash
kexec --load-hardboot /data/boot_images/ubuntu/kernel \
      --initrd=/data/boot_images/ubuntu/initrd.img \
      --mem-min=0x0 \
      --command-line="$(cat /proc/cmdline)"
```

### 4. Execution

```bash
kexec -e
```

Device performs warm reboot directly into Ubuntu kernel, bypassing bootloader.

## Build Integration

### device.mk

Ubuntu Touch build is already enabled in `device/xiaomi/perseus/device.mk`:

```makefile
# Ubuntu Touch multi-boot support
$(call inherit-product, device/xiaomi/perseus/ubuntu/ubuntu.mk)
```

### ubuntu/ubuntu.mk

```makefile
# Ubuntu Touch Build Configuration
# Enables the automatic generation of boot-ubuntu.img

PRODUCT_PROPERTY_OVERRIDES += \
    ro.ubuntu.enabled=1 \
    ro.ubuntu.device=perseus

PRODUCT_PACKAGES += \
    boot-ubuntu.img
```

### ubuntu/Android.mk

Builds boot-ubuntu.img automatically:

```makefile
LOCAL_MODULE := boot-ubuntu.img
UBUNTU_RAMDISK := $(LOCAL_PATH)/ramdisk/halium.cpio.gz
UBUNTU_CMDLINE := "console=ttyMSM0,115200n8 androidboot.hardware=qcom security=apparmor systemd.unified_cgroup_hierarchy=0"

$(LOCAL_BUILT_MODULE): $(INSTALLED_KERNEL_TARGET) $(UBUNTU_RAMDISK)
	$(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
	    --ramdisk $(UBUNTU_RAMDISK) \
	    --cmdline $(UBUNTU_CMDLINE) \
	    --output $@
```

### kaos.mk

nh-ubuntu script included:

```makefile
PRODUCT_COPY_FILES += \
    kaos/scripts/nh-ubuntu:$(TARGET_COPY_OUT_VENDOR)/bin/nh-ubuntu
```

## Usage Guide

### For Developers

1. **Verify Halium ramdisk present**
   ```bash
   ls -la device/xiaomi/perseus/ubuntu/ramdisk/halium.cpio.gz
   ```

2. **Build ROM**
   ```bash
   source build/envsetup.sh
   lunch lineage_perseus-userdebug
   mka bacon
   ```

3. **Verify boot-ubuntu.img and extracted files**
   ```bash
   ls -la out/target/product/perseus/data/boot_images/ubuntu/
   # Should show:
   # boot-ubuntu.img
   # kernel
   # initrd.img
   ```

4. **Flash to device**
   ```bash
   fastboot flash boot out/target/product/perseus/boot.img
   fastboot flash system out/target/product/perseus/system.img
   fastboot flash vendor out/target/product/perseus/vendor.img
   fastboot flash userdata out/target/product/perseus/userdata.img
   fastboot reboot
   # All Ubuntu files already in /data/boot_images/ubuntu/
   ```

### For End Users

1. **Verify Ubuntu support**
   ```bash
   adb shell
   getprop ro.ubuntu.enabled
   # Should return: 1
   ```

2. **Check boot-ubuntu.img and extracted files**
   ```bash
   adb shell
   su
   ls -la /data/boot_images/ubuntu/
   # Should show:
   # boot-ubuntu.img
   # kernel  
   # initrd.img
   ```

3. **Boot Ubuntu Touch**
   ```bash
   su
   nh-ubuntu boot
   # Device reboots into Ubuntu Touch
   ```

4. **Return to Android**
   ```bash
   # From Ubuntu Touch
   sudo reboot
   # Device boots back to Android
   ```

## Advantages

### ✅ Safe
- Android boot partition never modified
- No partition table changes
- Easy rollback (just reboot)
- Ubuntu uses same unified kernel as Android

### ✅ Automatic
- boot-ubuntu.img built automatically during ROM build
- Single script handles everything
- Automatic extraction
- No manual kernel/initrd management

### ✅ Integrated
- Uses existing Ubuntu Touch build infrastructure
- Halium ramdisk already present
- Unified kernel approach
- Property-based detection

### ✅ Simple
- Single command (`nh-ubuntu boot`) to switch OS
- Status checking built-in
- Help system included
- Clear error messages

## Requirements

### Build Requirements
- Halium ramdisk ✅ (already present at device/xiaomi/perseus/ubuntu/ramdisk/halium.cpio.gz)
- Ubuntu Touch build enabled ✅ (inherited in device.mk)
- kexec binary ✅ (already included in Kaos)
- Unified kernel ✅ (same kernel for Android and Ubuntu)

### Runtime Requirements
- Kexec-enabled kernel ✅ (CONFIG_KEXEC=y)
- Root access ✅ (Kaos provides)
- ~50MB free space in /data
- magiskboot ✅ (for extraction, usually present if Magisk installed)

### Manual Extraction (if magiskboot unavailable)
```bash
# Use tools like unpack_bootimg, abootimg, or mkbootimg
unpack_bootimg --boot_img /data/boot_images/ubuntu/boot-ubuntu.img
# Kernel and ramdisk already extracted during build
# Located at /data/boot_images/ubuntu/kernel and initrd.img
```

## Comparison with Other Methods

| Method | Safety | Complexity | Flexibility | Our Choice |
|--------|--------|------------|-------------|------------|
| Partition Swap | ⚠️ Medium | High | Low | ❌ |
| Boot Image Swap | ⚠️ Medium | Medium | Medium | ❌ |
| Kexec (our method) | ✅ High | Low | High | ✅ |
| Container | ✅ High | High | Medium | Future |

## Integration with Kaos

### Quick Settings Tile (Future Enhancement)

```java
// Kaos tile menu
if (isUbuntuAvailable()) {
    menu.add("Boot to Ubuntu Touch");
}

private boolean isUbuntuAvailable() {
    return SystemProperties.get("ro.kaos.ubuntu.enabled", "false").equals("true");
}

private void bootToUbuntu() {
    ShellUtils.su("nh-ubuntu boot");
}
```

### Current Access
- Command line: `nh-ubuntu boot`
- ADB: `adb shell su -c 'nh-ubuntu boot'`
- Termux/Kaos Terminal: Direct execution

## Troubleshooting

### Ubuntu boot.img not found
```bash
# Check if built
ls -la out/target/product/perseus/data/boot_images/ubuntu/boot-ubuntu.img

# Check if on device
adb shell ls -la /data/boot_images/ubuntu/

# If missing, check halium ramdisk
ls -la device/xiaomi/perseus/ubuntu/ramdisk/halium.cpio.gz

# Rebuild if needed
mka boot-ubuntu.img ubuntu-kernel ubuntu-initrd
```

### Extraction fails
```bash
# Check magiskboot
ls -la /data/adb/magisk/magiskboot

# Manual extraction:
# 1. Download boot image tools
# 2. Extract kernel and ramdisk
# 3. Place in /data/ubuntu-boot/
```

### Kexec fails
```bash
# Check kernel support
cat /proc/config.gz | gunzip | grep KEXEC
# Should show: CONFIG_KEXEC=y

# Check kexec binary
/vendor/bin/kexec --version
```

### Device doesn't boot Ubuntu
```bash
# Check kernel loaded
cat /sys/kernel/kexec_loaded
# Should be: 1

# Check boot files
ls -la /data/boot_images/ubuntu/
# Should have: boot-ubuntu.img, kernel, initrd.img
```

## Limitations

1. **Ubuntu Touch rootfs separate**
   - Boot.img only contains kernel/initrd
   - Ubuntu rootfs must be on /data or userdata partition
   - Follow UBports installation guide for rootfs setup

2. **One-way boot**
   - Android → Ubuntu: kexec
   - Ubuntu → Android: full reboot (no reverse kexec yet)

3. **Kernel compatibility**
   - Ubuntu kernel must support Perseus hardware
   - Halium kernel recommended
   - May need device-specific patches

## Future Enhancements

### Short Term
- [ ] Quick Settings tile integration
- [ ] Boot confirmation dialog
- [ ] Automatic rootfs detection

### Medium Term
- [ ] Multiple OS selection menu
- [ ] Sailfish OS support (same mechanism)
- [ ] postmarketOS support

### Long Term
- [ ] Reverse kexec (Ubuntu → Android without full reboot)
- [ ] Container-based approach (run Ubuntu in chroot/container)
- [ ] Convergence mode (both OS simultaneously)

## Testing Checklist

- [ ] Build ROM with ubuntu components
- [ ] Verify out/target/product/perseus/data/boot_images/ubuntu/ contains:
  - [ ] boot-ubuntu.img
  - [ ] kernel
  - [ ] initrd.img
- [ ] Flash ROM to device  
- [ ] Verify /data/boot_images/ubuntu/ on device
- [ ] Run `nh-ubuntu status` (should show all 3 files)
- [ ] Run `nh-ubuntu load`
- [ ] Run `nh-ubuntu exec` (boot Ubuntu - if rootfs available)
- [ ] Verify Ubuntu Touch boots
- [ ] Reboot to Android

## References

- Kexec implementation: `kaos/scripts/nh-kexec`
- Sailfish dual boot: `kaos/SAILFISH_DUAL_BOOT.md`
- Ubuntu Touch: https://ubuntu-touch.io/
- Halium: https://halium.org/
- UBports: https://ubports.com/

## Credits

- Based on Sailfish OS dual boot implementation
- Uses kexec from kexec-tools project
- Ubuntu Touch from UBports community
- Halium project for device adaptation

---

**Status**: Implementation Complete  
**Last Updated**: 2024-01-05  
**Next Steps**: Test with actual Ubuntu Touch boot.img
