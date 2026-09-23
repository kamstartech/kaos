#
# Kaos distro profile: droidian
#
# Droidian is a Halium/Android-in-LXC distro: it mounts the Android
# partitions itself from block devices inside the LXC container
# (lxc-android-config), so the trampoline must NOT bind-mount them host-side
# and must apply the systemd-unified-cgroup cmdline override.
#
# Mirrors what used to be decided by type=halium in distros.conf
# (trampoline.c L367, L539, L598). PROFILE_HALIUM replaces the type= check.

PROFILE_MOUNTS_HOST=""
PROFILE_UI="phosh"
PROFILE_KAOS_INIT="1"
PROFILE_MASK_EXTRA=""
PROFILE_PRESERVE_HAL="0"
PROFILE_INIT_PRIORITY=""
PROFILE_INSTALL_WRAPPERS="0"
PROFILE_HALIUM="1"
PROFILE_GFX="hwcomposer"
