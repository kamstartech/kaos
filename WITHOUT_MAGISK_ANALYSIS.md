# Kaos Root Server Bridge Architecture

## Overview

This ROM uses a **Root Server Bridge** architecture that provides root access without a traditional su binary. This makes root completely invisible to detection methods.

## How It Works

### Root Bridge Daemon (kaos-bridge)
- Runs as init service with root privileges
- Listens on localhost:30000
- Spawns root shell for incoming connections

### Root Client (kaos)
- Connects to localhost:30000
- Pipes commands to root shell
- Acts as su replacement for apps

### Flow
```
App/Script → kaos → localhost:30000 → kaos-bridge → root shell
```

## Why This Works

### No su Binary Detection
Traditional root detection looks for:
- `/system/xbin/su` - **Does not exist**
- `/system/bin/su` - **Does not exist**
- `/sbin/su` - **Does not exist**
- `which su` - **Returns nothing**

### SELinux Spoofing
- `getenforce` returns "Enforcing"
- Apps see a "secure" device

## Compatibility

| Feature | Status |
|---------|--------|
| Root access | ✅ Via kaos |
| Banking apps | ✅ Work normally |
| Streaming apps | ✅ Work normally |
| Games | ✅ Work normally |
| Kaos tools | ✅ Full functionality |
| Kali chroot | ✅ Works |

## Usage

```bash
# Execute command as root
kaos id

# Interactive root shell
kaos

# Kali chroot
kaos-starter
```

---

**Last Updated**: 2026-01-25
