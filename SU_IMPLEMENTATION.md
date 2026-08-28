# Kaos Root Implementation

## Overview

This document describes how root access is implemented in the Kaos ROM using a **Root Server Bridge** architecture instead of a traditional su binary.

## Architecture

### Design Goals
1. ✅ Root access invisible to detection methods
2. ✅ Banking apps, streaming, games all work normally
3. ✅ No su binary in standard locations
4. ✅ Compatible with all Kaos scripts and apps
5. ✅ Bypasses Android 14/15 Zygote restrictions

### Implementation Choice

**Using**: Root Server Bridge (netcat-based)
**NOT using**: Traditional su binary (detectable by apps)

## Components

### 1. kaos-bridge (Root Server)
**Location**: `/system/bin/kaos-bridge`

```bash
#!/system/bin/sh
# Listens on port 30000 and provides a root shell interface

PORT=30000
ADDR=127.0.0.1

# Start listener - binds incoming connections to root shell
/system/bin/nc -L -s $ADDR -p $PORT /system/bin/sh
```

**Purpose**:
- Runs as root via init service
- Listens on localhost:30000
- Spawns root shell for incoming connections
- Cannot be detected by standard root detection

### 2. kaos (Root Client)
**Location**: `/system/bin/kaos`

```bash
#!/system/bin/sh
# Connects to the kaos-bridge service for root access

exec /system/bin/nc 127.0.0.1 30000
```

**Purpose**:
- Acts like `su` for Kaos app
- Connects to root server on port 30000
- Pipes commands to root shell

### 3. kaos-service (Kali Namespace Manager)
**Location**: `/system/bin/kaos-service`

**Purpose**:
- Manages Kali Linux PID namespace
- Uses `unshare` for process isolation
- Bind mounts /dev, /sys, /proc into chroot

## Runtime Flow

### 1. Boot Time

**File**: `init.kaos.rc`

```
on property:sys.boot_completed=1
    start kaos-setup
    start kaos-chroot-install
    start kaos-mounts
    start kaos-bridge      # Starts kaos-bridge
    start kaos-ssh
```

### 2. Root Access Flow

```
App/Script
    ↓
kaos (connects to localhost:30000)
    ↓
kaos-bridge (running as root)
    ↓
/system/bin/sh (root shell)
    ↓
Command executes as root
```

### 3. Usage Examples

```bash
# Execute single command as root
echo "id" | kaos
# Output: uid=0(root) gid=0(root)

# Interactive root shell
kaos

# From Kaos app
# App uses kaos internally for root commands
```

## Why This Works

### No su Binary Detection

Traditional root detection looks for:
- `/system/xbin/su`
- `/system/bin/su`
- `/sbin/su`
- `/vendor/bin/su`
- `which su`

**Our implementation**: None of these exist!

Root apps detect nothing because:
1. No su binary in any standard location
2. No Magisk files or directories
3. No SuperSU artifacts
4. SELinux appears as enforcing

### SELinux Spoofing

**File**: `/system/bin/getenforce_fake`

Returns "Enforcing" regardless of actual state.
Apps checking SELinux status see a "secure" device.

### Bypasses Zygote Restrictions

Android 14/15 blocks setuid execution from app contexts.
Our solution:
- Root server runs as init service (not app context)
- Apps connect via socket, not setuid
- Clean architecture that bypasses restrictions

## Comparison with Alternatives

### vs Traditional su Binary
| Feature | Root Bridge | Traditional su |
|---------|-------------|----------------|
| Detection | ❌ Undetectable | ✅ Easily detected |
| Banking apps | ✅ Work | ❌ Blocked |
| Implementation | Socket-based | Setuid binary |
| Android 14/15 | ✅ Works | ⚠️ Restricted |

### vs Magisk
| Feature | Root Bridge | Magisk |
|---------|-------------|--------|
| Detection | ❌ Undetectable | ⚠️ Can be detected |
| Complexity | Simple | Complex |
| Modules | ❌ None | ✅ Supported |
| Updates | ❌ ROM only | ✅ Standalone |

## Init Service Configuration

**File**: `init.kaos.rc`

```
service kaos-bridge /system/bin/kaos-bridge
    class late_start
    user root
    group root
    seclabel u:r:init:s0
    disabled

on property:sys.boot_completed=1
    start kaos-bridge
```

## Troubleshooting

### Root server not running
```bash
# Check if running
ps -A | grep kaos-bridge

# Check port
netstat -tlnp | grep 30000

# Manually start
/system/bin/kaos-bridge &
```

### kaos not working
```bash
# Test connection
echo "id" | nc 127.0.0.1 30000

# Check kaos exists
ls -la /system/bin/kaos
```

### Apps still detecting root
```bash
# Check for leftover su binaries
ls /system/xbin/su /system/bin/su /sbin/su

# These should all return "No such file or directory"
```

## SSH Server

**File**: `/system/bin/kaos-ssh-server`

Runs sshd inside Kali chroot on port 2022.

```bash
# Access via ADB port forward
adb forward tcp:2022 tcp:2022
ssh root@localhost -p 2022
```

## References

- Init script: `kaos/init.kaos.rc`
- Server script: `kaos/scripts/kaos-bridge`
- Bridge script: `kaos/scripts/kaos`
- Service manager: `kaos/scripts/kaos-service`
- Kaos app: `kaos/apps/`

---

**Last Updated**: 2026-01-25
**ROM Version**: LineageOS 22.1
**Device**: Perseus (Xiaomi Mi MIX 3)
**Android Version**: 15
