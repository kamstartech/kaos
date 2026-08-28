# Kaos Android Apps Root Access

## Root Server Bridge Architecture

This ROM uses a **Root Server Bridge** for root access instead of a traditional su binary.

### How It Works

```
App → kaos → localhost:30000 → kaos-bridge → root shell
```

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| kaos-bridge | /system/bin | Root bridge daemon (init service) |
| kaos | /system/bin | CLI client to connect to bridge |

## App Compatibility

### legacy NetHunter apps

| App | Status | Notes |
|-----|--------|-------|
| com.kaos | ✅ Works | Uses kaos internally |
| com.offsec.nethunter.kex | ✅ Works | VNC server in chroot |
| com.offsec.nethunter.store | ✅ Works | Store functionality |
| com.kali.nethunter.tile | ✅ Works | Quick Settings tile |

### Third-Party Root Apps

Apps expecting traditional `su` binary **will NOT work** because:
- No `/system/xbin/su`
- No `/system/bin/su`
- No `su` in PATH

**This is by design** - it prevents root detection by banking apps and games.

### How to Use Root

```bash
# Execute command as root
kaos whoami
# Output: root

# Interactive shell
kaos
```

## Banking/Streaming App Compatibility

| App Type | Status | Reason |
|----------|--------|--------|
| Banking apps | ✅ Works | No su binary to detect |
| Streaming (Netflix, etc.) | ✅ Works | SELinux appears enforcing |
| Games with anti-cheat | ✅ Works | No root indicators |

---

**Last Updated**: 2026-01-25
