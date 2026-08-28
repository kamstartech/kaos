# Kaos Integration

> Device-agnostic distro boot subsystem. The original bring-up was done on the Xiaomi Mi MIX 3 (`perseus`), so some examples still reference that device.

**Kernel Status**: ✅ **VERIFIED ON DEVICE** - All Kaos requirements confirmed on actual hardware (ae712b83)

## Overview

This ROM includes Kali Kaos with namespace-isolated Linux environment and a native display pipeline. Root access is provided via a secure Root Bridge architecture invisible to standard detection methods.

### Architecture

**Root Bridge** (Invisible to apps):
- `kaos-bridge`: Root bridge daemon on localhost:30000 (`u:r:init:s0`)
- `kaos`: Unified CLI client that connects to the bridge
- Apps and scripts use `kaos` to execute commands as root
- Bypasses Android 14/15 Zygote capability restrictions
- **No su binary** = Banking apps, streaming, games all work normally

**SELinux Spoofing**:
- SELinux appears as **enforcing** to all detection methods
- Root detection checks pass because there's no su in standard locations
- Compatible with SafetyNet basic attestation

**Namespace Isolation**:
- `kaos-service`: Manages PID/Mount namespace via `unshare -m -p -f`
- Real PID 1 inside namespace (`kaos-init`)
- Full Linux environment with direct hardware access (/dev, /sys, /proc)

**Native Display Pipeline** (Phosh/Wayland):
- Phoc headless compositor with pixman renderer (no GPU/DRM needed)
- Phosh mobile shell (lock screen, app launcher)
- wf-recorder → ffmpeg → Unix socket streaming to DirectKeXActivity
- Touch input via `kaos-wl-input` (wlr-virtual-pointer Wayland protocol)

**Boot Services** (started on `sys.boot_completed=1`):
- `kaos-bridge`: Root bridge daemon (port 30000)
- `kaos-initializer`: Auto-starts namespace via `kaos-starter auto-boot`

### Core Binaries (installed to `/system/bin/`)
| Binary | Purpose |
|--------|---------|
| `kaos` | Unified CLI client (bridge client) |
| `kaos-bridge` | Root bridge daemon (port 30000) |
| `kaos-service` | Namespace lifecycle controller |
| `kaos-starter` | User-facing CLI for start/stop/status |
| `kaos-chroot-install` | Rootfs bootstrap installer |
| `kaos-ssh` | SSH server management |

### Kernel Features ✅ VERIFIED ON DEVICE
All configurations verified on device ae712b83:
- **Kexec Support**: CONFIG_KEXEC=y ✅
- **USB HID Gadget**: CONFIG_USB_CONFIGFS_F_HID=y (for HID attacks) ✅
- **Wireless Injection**: MAC80211, CFG80211 with WEXT ✅
- **Network Packet Filtering**: Complete Netfilter/iptables support ✅
- **TUN/TAP**: For VPN and tunneling ✅
- **Loadable Modules**: CONFIG_MODULES=y ✅
- **SELinux**: Full support with boot-time control ✅
- **Wireless Drivers**: ATH10K, RTL8192CU, RT2800USB (as modules) ✅

### Current Status
✅ **Kernel**: Kaos-ready (all required configs verified)
✅ **Root Bridge**: Always-on via kaos-bridge (invisible to detection)
✅ **SELinux Spoofing**: Appears enforcing to all apps
✅ **Banking/Streaming**: Works normally (no su binary to detect)
✅ **Kali Chroot**: Auto-installed from bootstrap system
✅ **SSH Server**: Available on port 22 (native integration)
✅ **Kaos app**: Built from source, pre-installed
✅ **Kaos Terminal**: Pre-installed (default shell: `/system/bin/kaos`)
✅ **Phosh Display**: Headless Wayland pipeline streaming to Android
✅ **Touch Input**: Wayland virtual pointer injection working
🔄 **Keyboard Input**: Virtual keyboard protocol integration in progress

### Implementation Details
- **Root Access**: Via `kaos` CLI connecting to bridge on localhost:30000
- **Kali Location**: `/data/local/nhsystem/kalifs` (bind mount with suid from `kali-arm64`)
- **SSH Port**: 22 (native, forwarded via `adb forward tcp:2222 tcp:22`)
- **Namespace Manager**: `kaos-starter {start|stop|restart|status}`
- **Display Socket**: `/dev/socket/kaos_ui.sock` (RGBA video stream)
- **Input Socket**: `/dev/socket/kaos_input.sock` (touch events)

## Using Root Access

### Via kaos CLI
```bash
# Execute a command as root
kaos id
# Output: uid=0(root) gid=0(root)

# Interactive root shell (inside namespace)
kaos
```

### Kali Namespace Management
```bash
# Check namespace status (via bridge)
adb shell "echo '/system/bin/kaos-service --distro kali status' | nc 127.0.0.1 30000"

# Start Kali namespace
adb shell "echo '/system/bin/kaos-service --distro kali start' | nc 127.0.0.1 30000"

# Login to running namespace
adb shell "/system/bin/kaos-starter --distro kali exec /bin/bash"

# Stop namespace
adb shell "echo '/system/bin/kaos-service --distro kali stop' | nc 127.0.0.1 30000"
```

> **Note on init**: The Mi Mix 3 runs kernel 4.9. Systemd 259 (Kali 2026.x) requires
> cgroup v2 delegation not available on this kernel. The namespace uses `kaos-init`
> (Phosh/Wayland stack) instead of systemd — the namespace stays stable and all CLI
> tools work normally via `exec`/`login`.

### SSH Access (inside namespace)
```bash
# SSH is available on port 22 (inside namespace)
# Forward via ADB:
adb forward tcp:2222 tcp:22

# Then connect from host:
ssh root@localhost -p 2222
# Password: toor
```

## Adding Kaos Components

### 1. Kaos App ✅ BUILT FROM SOURCE
The Kaos app is **built from source** and **pre-installed** in the ROM.

**Source Location**: `kaos/apps/phosh-app/`

### 2. Kaos Terminal ✅ PRE-INSTALLED
The Kaos Terminal is included as a prebuilt APK.

### 3. Kali Chroot ✅ ON-DEMAND INSTALL
The Kali rootfs is installed on demand via the Phosh app or manually via the root bridge.

**Location:** `/data/.stowaway/kali/` (HybridOS mode)  
**Legacy symlink:** `/data/local/nhsystem/kali-arm64` (for app compatibility)

**Manual install:**
```bash
# Download Kali minimal rootfs (~130 MB)
# Then install via bridge:
adb shell "echo '/system/bin/kaos-chroot-install --distro kali -c restore /path/to/kali-rootfs.tar.xz /data/.stowaway/kali' | nc 127.0.0.1 30000"
```

## Building with Kaos

Simply build the ROM as normal:
```bash
source build/envsetup.sh
lunch lineage_perseus-userdebug
mka bacon
```

The Kaos components will be automatically included.

## Post-Install

After flashing the ROM:
1. Boot into system
2. Kaos app is **already installed** as a system app
3. Kali chroot auto-extracts on first boot (may take a few minutes)
4. Root is always available via `kaos`
5. Open Kaos app to access tools
6. Start hacking!

## Features Available

- ✅ Banking apps work (no su binary to detect)
- ✅ Streaming apps work (Netflix, etc.)
- ✅ Games with anti-cheat work
- ✅ Kaos app has full root access via bridge
- ✅ All pentesting tools available

### Pentesting Features
- **HID Keyboard Attacks**: Use your phone as a USB keyboard
- **BadUSB**: Rubber Ducky attacks
- **MITM Framework**: Ettercap, mitmproxy
- **Wireless Attacks**: Airodump-ng, aircrack-ng (with compatible adapter)
- **Network Scanning**: Nmap, Masscan
- **Exploitation Tools**: Metasploit Framework
- **Custom Kernels**: kexec support for booting custom kernels
- **SSH Server**: Remote access on port 2022

### kexec Binary ✅ INTEGRATED

The kexec binary is included for advanced kernel switching without full device reboot.

**Location**: `/vendor/bin/kexec`

**Helper Script**:
```bash
nh-kexec status              # Check status
nh-kexec load /sdcard/kernel # Load kernel
nh-kexec exec                # Execute loaded kernel
nh-kexec reboot /sdcard/kernel # Load and execute
```

**Use Cases**:
- MultiROM: Boot multiple ROMs/kernels
- Testing custom kernels without flashing
- Pentesting with specialized kernels
- Quick kernel switching for different attack scenarios

## Supported USB Wireless Adapters

The kernel includes drivers for:
- Atheros ATH10K (module)
- Realtek RTL8192CU (module)
- Ralink RT2800USB (module)

External adapters supporting monitor mode will work!

## Troubleshooting

### Root Bridge Not Running
```bash
# Check if kaos-bridge is running
ps -ef | grep kaos-bridge

# Check if port 30000 is listening
netstat -tlnp | grep 30000

# Manually start
/system/bin/kaos-bridge &
```

### Namespace Issues
```bash
# Check service status
kaos-starter status

# Check logs inside namespace
adb forward tcp:2222 tcp:22
ssh root@localhost -p 2222 "cat /var/log/kaos-init.log"

# Manually restart
kaos-starter restart
```

### Display Not Working
```bash
# Inside namespace (via SSH):
# Check if phoc is running
pgrep phoc

# Check if wf-recorder is streaming
pgrep wf-recorder

# Check logs
cat /var/log/phosh.log
cat /var/log/wf.log
```

### Touch Input Not Working
```bash
# Inside namespace:
# Check if wl-input is running
pgrep kaos-wl-input

# Check logs
cat /var/log/wl-input.log

# Verify wayland socket exists
ls -la /run/user/0/wayland-0
```

## Security Notice

⚠️ **Warning**: Kaos includes penetration testing tools.
- Only use on networks you own or have permission to test
- This is for educational and authorized security testing only
- Unauthorized use may be illegal in your jurisdiction

**Note**: This ROM uses a Root Server Bridge architecture - there is no `su` binary in standard locations. Banking apps, streaming services, and games work normally because root detection methods cannot find traditional root indicators.

## How It Works

### Root Bridge Architecture
1. **kaos-bridge**: Root daemon running as `u:r:init:s0` on localhost:30000
2. **kaos**: CLI client connects to bridge, executes commands as root
3. **No su binary**: Apps cannot detect root via standard methods
4. **SELinux Spoofing**: Always appears as enforcing to detection

### Boot Sequence
1. System boots normally
2. `kaos-bridge` starts root bridge on port 30000
3. `kaos-initializer` runs `kaos-starter auto-boot`
4. Namespace created via `unshare -m -p -f` with `kaos-init` as PID 1
5. Inside namespace: dbus, SSH, Phosh display pipeline, wl-input all start
6. DirectKeXActivity connects to display/input sockets

## Credits

- Kali NetHunter Team: https://www.kali.org/docs/nethunter/
- LineageOS Team
- Xiaomi Mi MIX 3 Community

## License

This integration follows the same license as LineageOS (Apache 2.0)
