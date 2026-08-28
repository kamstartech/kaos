# Kaos Root Access Audit

## Current Implementation

The Kaos ROM uses a **Root Server Bridge** architecture for root access.

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| kaos-bridge | /vendor/bin | Root server on localhost:30000 |
| kaos | /system/bin | Client to connect to root server |
| kaos-service | /vendor/bin | Kali namespace manager |

### Root Detection Status

**Not Detectable Because**:
- ❌ No `/system/xbin/su`
- ❌ No `/system/bin/su`
- ❌ No `/sbin/su`
- ❌ No Magisk files
- ❌ No SuperSU artifacts
- ✅ SELinux appears as enforcing

### Compatibility

| App Type | Status | Notes |
|----------|--------|-------|
| Banking apps | ✅ Works | No su binary to detect |
| Streaming (Netflix, etc.) | ✅ Works | No root indicators |
| Games with anti-cheat | ✅ Works | SELinux appears enforcing |
| Kaos app | ✅ Works | Uses kaos for root |
| Root apps (expecting su) | ⚠️ May fail | No traditional su binary |

### Scripts Using Root

All Kaos scripts use `kaos` for root access:

| Script | Root Method | Status |
|--------|-------------|--------|
| kaos-service | Init context (already root) | ✅ |
| kaos-starter | Init context | ✅ |
| kaos-ssh-server | Init context | ✅ |
| nh-kexec | Via kaos | ✅ |
| kaos-service detect | No root needed | ✅ |

### How to Get Root

```bash
# Via kaos
echo "id" | kaos
# Output: uid=0(root) gid=0(root)

# Interactive shell
kaos
```

---

**Last Updated**: 2026-01-25
