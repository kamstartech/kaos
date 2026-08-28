# Kali NetHunter Chroot Integration

This directory contains scripts and configuration for integrating Kali Linux chroot with the Perseus Kaos build.

## Chroot Options

NetHunter supports installing a Kali Linux chroot environment on the device. There are three variants:

### 1. **Nano** (~50MB compressed, ~150MB extracted)
- Minimal Kali tools
- Best for devices with limited storage
- Basic penetration testing tools

### 2. **Minimal** (~140MB compressed, ~500MB extracted)  
- Standard NetHunter tools
- Recommended for most users
- Good balance of tools and size

### 3. **Full** (~1.5GB compressed, ~5GB extracted)
- Complete Kali Linux environment
- All penetration testing tools
- Requires significant storage space

## Installation Methods

### Method 1: Via NetHunter App (Recommended)
The NetHunter app can download and install the chroot automatically:
1. Flash ROM with Kaos integration
2. Install NetHunter app
3. Open app → Kali Chroot Manager
4. Select variant (nano/minimal/full)
5. Download and install

### Method 2: Pre-integrated in ROM (Advanced)

To embed the chroot in the ROM build:

1. **Download chroot manually:**
```bash
# From the NetHunter project or mirrors
# Note: Official download links change; check NetHunter documentation
wget https://example.com/kalifs-arm64-minimal.tar.xz
```

2. **Verify checksum:**
```bash
sha256sum kalifs-arm64-minimal.tar.xz
```

3. **Place in build:**
```bash
cp kalifs-arm64-minimal.tar.xz kaos/chroot/
```

4. **Update build configuration:**
Edit `kaos/kaos.mk` to include chroot installation

## Build Integration (TODO)

The following files need to be created to embed chroot in ROM:

- `init.chroot.rc` - Init script to extract and setup chroot at boot
- `chroot_install.sh` - Script to install chroot to `/data/local/nhsystem/`
- `Android.mk` updates - Copy chroot archive to /system or /vendor

## Current Status

✅ Kaos scripts installed  
✅ Root hiding mechanism working
✅ Init system configured
⏳ Chroot download/integration (user must install via app)

## References

- NetHunter Documentation: https://www.kali.org/docs/nethunter/
- NetHunter GitLab: https://gitlab.com/kalilinux/nethunter
- Build Scripts: https://gitlab.com/kalilinux/build-scripts/kali-nethunter-project
