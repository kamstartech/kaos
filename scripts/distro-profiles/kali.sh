#
# Kaos distro profile: kali
#
# Captures the Kali-specific branches that used to live inline:
#
#   1. ns-entry Phase 5 init candidate order — kaos-init wins when /sbin/init
#      is not a systemd symlink (old NetHunter-style chroots) L1492-1502
#   2. kaos-chroot-install install_kali_boot_wrappers() L40-93 — deploys
#      kali-preinit at /sbin/preinit plus kali-hybridos-* systemd units
#
# Also matched by normalize_distro_name(1) for aliases kali-pro, kali_native,
# kali-native, and by distros-lib's no-config Kali fallback.

PROFILE_MOUNTS_HOST=""
PROFILE_UI="phosh"
PROFILE_KAOS_INIT="1"
PROFILE_MASK_EXTRA=""
PROFILE_PRESERVE_HAL="0"
PROFILE_INIT_PRIORITY="/usr/local/sbin/kaos-init /sbin/init /usr/lib/systemd/systemd"
PROFILE_INSTALL_WRAPPERS="1"
PROFILE_GFX="mesa"
