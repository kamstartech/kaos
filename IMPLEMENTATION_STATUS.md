# HybridOS Implementation Status — Xiaomi Mi Mix 3 (Perseus)

**Project Architecture**: HybridOS (Concurrent Android + Native Linux)
**Last Audit**: April 11, 2026
**Status**: ✅ **PRODUCTION READY — CORE ARCHITECTURE**

## 1. System Integration (Android Side)
- **Base ROM**: LineageOS 22 (Android 15)
- **Init Integration**: `init.kaos.rc` manages service chaining.
- **Root Bridge**: `kaos-bridge` on Port 30000 (u:r:init:s0 context).
- **Service Control**: `kaos-service` (lifecycle) + `kaos-starter` (CLI).
- **Multi-Distro**: `distros-lib.sh` + `distros.conf` registry.

## 2. Linux Namespace Features
- **PID 1 Init**: `kaos-init` (Phosh/Wayland stack) on kernel 4.9; systemd on kernel ≥5.0.
  > **Note**: Systemd 259 (Kali 2026.x) requires cgroup v2 delegation not available on the
  > Mi Mix 3's kernel 4.9. `kaos-service` auto-detects kernel version and uses
  > `kaos-init` or an idle PID 1 keeper on affected kernels.
- **Hardware Passthrough**: Direct bind-mounts for `/dev/dri`, `/dev/input`, and network interfaces.
- **Native Touch**: `kaos-wl-input` injecting events into Phoc via `wlr-virtual-pointer-unstable-v1`.
- **Keyboard**: Squeekboard (Native Linux OSK) fully integrated.
- **SSH Access**: Native on Port 22 (automated at boot).

## 3. Display Architecture
- Headless Phoc (unmodifiable stock apt binary) -> wf-recorder (wlr-screencopy) ->
  ffmpeg rawvideo relay -> `/dev/socket/kaos_ui.sock` -> `kaos-display-bridge`
  (GL texture blit) -> libhybris SF compat surface -> SurfaceFlinger -> display.
- `kaos-display-bridge` also owns `/dev/socket/kaos_display_ctl.sock`
  (SHOW/HIDE/QUIT from `PhoshDisplayActivity`, a transparent touch-forwarding
  overlay that does not render anything itself).
- `LINUX_DIRECT_MODE` (SailfishOS full mode) bypasses this pipeline entirely —
  phoc gets real `/dev/dri` access directly, `kaos-display-bridge` is not started.

## 4. Kernel Capabilities (`perseus.config`)
- ✅ **Namespaces**: PID, NET, USER, UTS, IPC, CGROUP.
- ✅ **Cgroups**: DEVICE, PIDS, MEMCG, CPUACCT, BLK_CGROUP.
- ✅ **Multi-OS**: SELinux (Android) + AppArmor (Ubuntu Touch) switchable via kexec/params.
- ✅ **Virtual Terminals**: `CONFIG_VT=y` for native console support.
- ✅ **Filesystems**: OverlayFS, SquashFS, FUSE, Loop (16 count).
- ⚠️ **Cgroup v2 delegation**: Not available on kernel 4.9 — systemd as PID 1 unsupported.

## 5. Verification Checklist
- [x] Root Bridge accessible via `nc 127.0.0.1 30000`
- [x] SELinux reports "Enforcing" while Kernel is Permissive
- [x] Kali Kaos 2026.1 namespace starts and remains stable (verified April 2026)
- [x] Kali namespace login via `kaos-starter --distro kali exec /bin/bash` works
- [x] Phosh UI reachable via Android app
- [x] Touch input correctly excluded from Android InputManager (`excluded-input-devices.xml`)
- [x] Distro installation pipeline (phosh app → bridge → kaos-chroot-install) verified end-to-end

## 📋 Remaining Work
- 🔄 **Docker/LXC**: Requires kernel rebuild with `CONFIG_BRIDGE_NETFILTER`.
- 🔄 **Systemd on kernel 4.9**: Would require backporting cgroup v2 nsdelegate support.
- 📋 **Multi-distro**: Arch, Alpine ✅, Ubuntu, Debian ready in distros.conf.
