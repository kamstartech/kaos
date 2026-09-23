#
# Kaos distro profile: sailfish
#
# Captures every SailfishOS-specific branch that used to live inline in
# kaos-service (formerly keyed off `grep -q 'sailfishos' /etc/os-release`):
#
#   1. do_mounts() firmware/system_root/linkerconfig/configfs block
#      (kaos-service L122-163)
#   2. Skip phosh UI setup (lipstick is the SFOS compositor) L263/L609
#   3. ns-entry Phase 4 mask preservation (SFOS_FULL) L1387/L1429
#
# All three now flow from PROFILE_* alone; no os-release sniffing remains.

PROFILE_MOUNTS_HOST="vendor/firmware_mnt vendor/dsp vendor/bt_firmware mnt/vendor/persist"
PROFILE_UI="sailfish"
PROFILE_KAOS_INIT="0"
PROFILE_MASK_EXTRA=""
PROFILE_PRESERVE_HAL="1"
PROFILE_INIT_PRIORITY=""
PROFILE_INSTALL_WRAPPERS="0"
PROFILE_GFX="none"
