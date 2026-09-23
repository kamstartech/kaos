#
# Kaos distro profile: ubuntu-touch (aka ubuntu-touch / ubuntu_touch aliases)
#
# Ubuntu Touch is a Halium/Android-in-LXC distro, same division of labor as
# Droidian: it mounts Android partitions itself inside the LXC container, so
# the trampoline skips host-side partition binds and applies the
# systemd-unified-cgroup cmdline override (trampoline.c L367, L539, L598).
#
# Matched by normalize_distro_name(1) for aliases ubuntu-touch, ubuntu_touch.

PROFILE_MOUNTS_HOST=""
PROFILE_UI="phosh"
PROFILE_KAOS_INIT="1"
PROFILE_MASK_EXTRA=""
PROFILE_PRESERVE_HAL="0"
PROFILE_INIT_PRIORITY=""
PROFILE_INSTALL_WRAPPERS="0"
PROFILE_HALIUM="1"
PROFILE_GFX="hwcomposer"
