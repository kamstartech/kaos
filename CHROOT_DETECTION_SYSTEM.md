# Kaos Chroot Detection & Status System

## Overview
A comprehensive system to **detect, monitor, and display** the Kaos chroot status in your ROM.

## Components Created

### 1. Detection Script ✅
**File**: `kaos-service detect`
**Location**: `/system/bin/kaos-service detect` (after build)

**Features:**
- Detects if chroot is installed
- Checks if chroot is mounted
- Gets chroot size, version, and variant
- Scans for available tools
- Sets system properties for status
- Creates status file for quick checks

**Usage:**
```bash
# Full status display
kaos-service detect

# Quick status only
kaos-service detect quick

# Set properties only (no output)
kaos-service detect props
```

### 2. Boot Service ✅
**File**: `kaos-detect.rc`
**Location**: `/vendor/etc/init/kaos-detect.rc` (after build)

**What it does:**
- Runs automatically at boot
- Detects chroot presence
- Sets system properties
- Updates status when chroot is mounted/unmounted

### 3. System Properties Set

After boot, these properties are available:

```bash
# Check if chroot is installed
getprop kaos.chroot.installed
# Returns: true or false

# Check if chroot is mounted
getprop kaos.chroot.mounted  
# Returns: true or false

# Get overall status
getprop kaos.chroot.status
# Returns: READY, NOT MOUNTED, or NOT INSTALLED

# Get chroot size
getprop kaos.chroot.size
# Returns: e.g., "512M" or "1.5G"

# Get Kali version
getprop kaos.chroot.version
# Returns: e.g., "2024.1"

# Get variant
getprop kaos.chroot.variant
# Returns: nano, minimal, or full

# Get number of tools found
getprop kaos.chroot.tools_count
# Returns: number

# Get general NH status
getprop kaos.status
# Returns: "Chroot Active", "Chroot Inactive", or "Chroot Not Installed"
```

## How to Check Chroot Status

### Method 1: Command Line
```bash
# Full detailed status
adb shell su 0 kaos-service detect

# Output example:
════════════════════════════════════════════════
  Kaos Chroot Detection & Status
════════════════════════════════════════════════

Status: READY

✓ Chroot directory exists: /data/local/nhsystem
✓ Chroot filesystems are mounted
ℹ Chroot size: 512M
ℹ Kali version: 2024.1
ℹ Variant: minimal

Available Kaos Tools:
─────────────────────────────────────────────
  ✓ aircrack-ng
  ✓ metasploit
  ✓ nmap
  ✓ sqlmap

Quick Commands:
─────────────────────────────────────────────
  $ kaos-starter shell          # Enter Kali Linux
  $ kaos-starter stop           # Unmount chroot
```

### Method 2: System Properties
```bash
# Quick check
adb shell getprop kaos.status

# Detailed check
adb shell getprop | grep kaos.chroot
```

### Method 3: Status File
```bash
# Read status file
adb shell cat /data/local/.kaos_status

# Output example:
Kaos Chroot Status
Generated: Sat Dec  7 20:30:15 UTC 2024

Installed: true
Mounted: true
Status: READY
Size: 512M
Version: 2024.1
Variant: minimal
Tools: 4
```

### Method 4: From Android App
Apps can check the properties:
```java
// In Android app code
SystemProperties.get("kaos.chroot.installed"); // "true" or "false"
SystemProperties.get("kaos.status"); // Current status
```

## Visual Indicators

### Terminal Output
```
✓ = Success (green)
⚠ = Warning (yellow)
✗ = Error (red)
ℹ = Information (blue)
```

### Status Values
```
READY           - Chroot installed and mounted
NOT MOUNTED     - Chroot installed but not active
NOT INSTALLED   - No chroot found
```

## Integration with ROM

### During Boot:
1. System boots
2. `kaos-detect` service starts
3. Runs `kaos-service detect props` (silent mode)
4. Sets all system properties
5. Creates status file

### When Chroot Changes:
1. User runs `kaos-starter start`
2. Init system detects property change
3. Updates `kaos.status` property
4. Apps/tiles can react to change

### For Developers:
```bash
# Check status in your scripts
if [ "$(getprop kaos.chroot.installed)" = "true" ]; then
    echo "Chroot is ready!"
else
    echo "Please install chroot"
fi

# Wait for chroot to be ready
while [ "$(getprop kaos.chroot.status)" != "READY" ]; do
    sleep 1
done
```

## Use Cases

### 1. Quick Status Check
```bash
$ kaos-service detect quick
Status: READY
```

### 2. Verify Before Running Tools
```bash
#!/system/bin/sh
# Your script that needs chroot

if [ "$(getprop kaos.chroot.installed)" != "true" ]; then
    echo "Error: Kaos chroot not installed"
    echo "Please install using the Kaos app"
    exit 1
fi

if [ "$(getprop kaos.chroot.mounted)" != "true" ]; then
    echo "Starting chroot..."
    kaos-starter start
fi

# Now run your tools
kaos-starter shell -c "aircrack-ng --help"
```

### 3. System Monitoring
```bash
# Watch chroot status in real-time
watch -n 1 'getprop | grep kaos'
```

### 4. Boot Notification
Add to init.rc:
```
on property:kaos.chroot.status=READY
    exec - root -- /system/bin/log -t Kaos "Kali chroot ready!"

on property:kaos.chroot.status=NOT\ INSTALLED
    exec - root -- /system/bin/log -t Kaos "Chroot not found - install via app"
```

## Status File Format

Location: `/data/local/.kaos_status`

```
Kaos Chroot Status
Generated: Sat Dec  7 20:30:15 UTC 2024

Installed: true
Mounted: true
Status: READY
Size: 512M
Version: 2024.1
Variant: minimal
Tools: 4
```

This file is:
- Created at boot
- Updated when chroot status changes
- Readable by all users (644 permissions)
- Machine-parseable

## Troubleshooting

### Chroot Shows as NOT INSTALLED but it exists
```bash
# Check directory
ls -la /data/local/nhsystem

# If exists but incomplete:
# - Missing files
# - Incomplete extraction
# - Wrong permissions

# Fix: Reinstall chroot
kaos-chroot-install
```

### Properties Not Set
```bash
# Manually run detection
kaos-service detect props

# Check if service is running
getprop | grep kaos

# Restart detection service
setprop ctl.restart kaos-detect
```

### Status File Missing
```bash
# Recreate
kaos-service detect props

# Check permissions
ls -la /data/local/.kaos_status
```

## Future Enhancements

### Potential Additions:
1. **Quick Settings Tile** for chroot status
2. **Notification** when chroot is ready
3. **Boot animation** showing detection
4. **Widget** displaying status on home screen
5. **Web dashboard** for remote monitoring

### Property Extensions:
```bash
kaos.chroot.last_used    # Timestamp
kaos.chroot.uptime       # How long mounted
kaos.chroot.errors       # Error count
kaos.chroot.auto_start   # Auto-mount on boot
```

## Files Modified

### New Files:
1. `kaos/scripts/kaos-service detect`
2. `kaos/init/kaos-detect.rc`

### Modified Files:
1. `kaos/kaos.mk`
   - Added kaos-service detect script
   - Added kaos-detect.rc init service

## Build Integration

After next ROM build, the system will have:

```
/system/bin/kaos-service detect                    # Detection script
/vendor/etc/init/kaos-detect.rc     # Boot service
/data/local/.kaos_status            # Status file (created at boot)
```

## Testing After Flash

```bash
# 1. Boot device and wait ~30 seconds

# 2. Check if detection ran
adb shell getprop kaos.status

# 3. Run manual detection
adb shell su 0 kaos-service detect

# 4. If chroot exists, should show:
#    ✓ Chroot directory exists
#    ✓ Filesystems mounted (if started)
#    ℹ Size, version, variant

# 5. If no chroot:
#    ✗ Chroot not found
#    Quick Commands: kaos-chroot-install
```

## Summary

**Problem**: No way to know if chroot is installed
**Solution**: Comprehensive detection and status system
**Result**: 
- ✅ Automatic detection at boot
- ✅ System properties for easy checking
- ✅ Status file for quick reference
- ✅ Command-line tool for detailed info
- ✅ Integration with init system

Now you can **always know** if Kaos chroot is present and ready! 🎉
