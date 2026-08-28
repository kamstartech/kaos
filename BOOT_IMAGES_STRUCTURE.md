# Multi-OS Boot Images Directory Structure

This directory contains pre-extracted boot components for different operating systems that can be loaded via kexec.

## Directory Structure

```
/data/boot_images/
├── ubuntu/
│   ├── boot-ubuntu.img  - Full Ubuntu Touch boot image
│   ├── kernel           - Ubuntu Touch kernel (pre-extracted)
│   └── initrd.img       - Ubuntu Touch initramfs (pre-extracted)
├── sailfish/            - (Future) Sailfish OS components
│   ├── boot-sailfish.img
│   ├── kernel
│   └── initrd.img
├── postmarket/          - (Future) postmarketOS components
│   ├── boot-postmarket.img
│   ├── kernel
│   └── initrd.img
└── README.md            - This file
```

## Benefits of Pre-Extraction

### ✅ Faster Boot Switching
- No runtime extraction needed
- Instant kexec loading
- No dependency on magiskboot or other extraction tools

### ✅ More Reliable
- Extraction done in controlled build environment
- No risk of extraction failures on device
- Consistent across all devices

### ✅ More Space Available
- /data partition is much larger than /vendor
- Can accommodate multiple OS boot images
- Typical /data: 50-100+ GB
- Typical /vendor: 1-2 GB (limited space)

### ✅ Consistent with Existing System
- ROM already uses /data/boot_images/ for boot-ubuntu.img
- Keeps all boot-related files in same location
- No fragmentation across partitions

### ✅ Multi-OS Ready
- Easy to add more operating systems
- Organized structure: /data/boot_images/{os_name}/
- Each OS isolated in its own directory
- Plenty of space for multiple OS

## Adding a New OS

To add support for another operating system (e.g., Sailfish OS):

1. **Create build module** in `device/xiaomi/perseus/{os_name}/Android.mk`:
   ```makefile
   # Build boot-{os_name}.img
   LOCAL_MODULE := boot-{os_name}.img
   
   # Extract kernel
   LOCAL_MODULE := {os_name}-kernel
   LOCAL_MODULE_PATH := $(TARGET_OUT_DATA)/boot_images/{os_name}
   
   # Extract initrd
   LOCAL_MODULE := {os_name}-initrd
   LOCAL_MODULE_PATH := $(TARGET_OUT_DATA)/boot_images/{os_name}
   ```

2. **Create helper script** `nethunter/scripts/nh-{os_name}`:
   ```bash
   OS_DIR=/data/boot_images/{os_name}
   OS_KERNEL=$OS_DIR/kernel
   OS_INITRD=$OS_DIR/initrd.img
   ```

3. **Add to device.mk**:
   ```makefile
   $(call inherit-product, device/xiaomi/perseus/{os_name}/{os_name}.mk)
   ```

## Current OS Support

| OS | Status | Helper Script | Directory |
|----|--------|---------------|-----------|
| **Ubuntu Touch** | ✅ Active | `nh-ubuntu` | `/data/boot_images/ubuntu/` |
| Sailfish OS | ⏳ Planned | `nh-sailfish` | `/data/boot_images/sailfish/` |
| postmarketOS | ⏳ Planned | `nh-postmarket` | `/data/boot_images/postmarket/` |

## File Sizes

Typical sizes for boot components:

- Kernel: ~15-30 MB (compressed)
- Initrd: ~5-15 MB (compressed)
- Total per OS: ~20-45 MB

With 3 OS options: ~60-135 MB total in /data

## Security

## Usage

Each OS has its own helper script:

```bash
# Ubuntu Touch
nh-ubuntu boot

# Sailfish OS (when implemented)
nh-sailfish boot

# postmarketOS (when implemented)
nh-postmarket boot
```

## Return to Android

From any OS, simply reboot:
```bash
sudo reboot
```

The bootloader will load Android boot.img from the boot partition.

---

**Last Updated**: 2024-01-05  
**Structure**: Multi-OS ready  
**Current**: Ubuntu Touch only
