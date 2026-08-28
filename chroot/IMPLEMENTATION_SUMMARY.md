# Kaos Kali Chroot Integration

## Overview

The Kali chroot is embedded in the ROM and auto-installed on first boot.

## Current Implementation

### Auto-Install at Boot
- Chroot tarball embedded at build time
- `kaos-chroot-install` service extracts on first boot
- Location: `/data/local/nhsystem/kalifs`

### Available Commands

```bash
# Check chroot status
kaos-starter status

# Enter chroot shell
kaos-starter

# Start Kali namespace
nh-service start

# Stop Kali namespace
nh-service stop
```

### SSH Access

SSH server runs inside chroot on port 2022:

```bash
# Forward via ADB
adb forward tcp:2022 tcp:2022

# Connect
ssh root@localhost -p 2022
```

## Files Structure

```
kaos/
├── chroot/
│   ├── kalifs-arm64-minimal.tar.xz  # Embedded chroot
│   └── README.md
├── scripts/
│   ├── kaos-starter                    # Chroot manager
│   ├── kaos-chroot-install            # Installation script
│   ├── nh-service                   # Namespace manager
│   └── kaos-ssh                # SSH starter
└── init.kaos.rc                # Boot services
```

---

**Last Updated**: 2026-01-25
