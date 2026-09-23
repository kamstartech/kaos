#
# Kaos distro profile: generic (default for any distro without a profile)
#
# A profile is a sourced shell file declaring the distro's boot behavior as
# plain variables. The loader (distros-lib.sh:load_distro_profile) sources a
# generic default, then overlays <normalized-name>.sh if present.
#
# Consumed by: kaos-service (namespace path), kaos-chroot-install,
# and — after Phase 2 — the trampoline's switch_root path.
#
# Phase 2 (Approach A): the distros.conf behavior keys are now authoritative.
# resolve_distro() reads them and distro_manifest() applies precedence
# distros.conf keys > this profile file > generic defaults. These profile files
# are therefore the fallback/example set (kept so legacy confs and unknown
# distros behave unchanged), not the primary runtime source.
#
# Variable contract:
#   PROFILE_MOUNTS_HOST     space list of extra Android->distro bind mounts
#                           beyond the generic set in do_mounts()
#   PROFILE_UI              phosh | sailfish | none
#   PROFILE_KAOS_INIT       0|1 — offer synthetic kaos-init as init fallback
#   PROFILE_MASK_EXTRA      space list of systemd units to mask (Phase 4)
#   PROFILE_PRESERVE_HAL    0|1 — keep droid-hal-init/sensorfwd/bluebinder
#                           (overrides MASK_EXTRA semantics for those units)
#   PROFILE_INIT_PRIORITY   ordered init candidates (first executable wins)
#   PROFILE_INSTALL_WRAPPERS 0|1 — install distro-specific PID1 preinit
#                           wrappers at rootfs (kaos-chroot-install)
#   PROFILE_HALIUM          0|1 — distro mounts Android partitions itself
#                           (lxc-android-config, Droidian/UT); trampoline
#                           skips its own partition binds and cmdline v2
#   PROFILE_GFX             hwcomposer | mesa | none — graphics backend:
#                           hwcomposer = Android hwcomposer via libhybris
#                           (Halium-style); mesa = KGSL Mesa/DRM; none =
#                           distro manages its own (default: mesa)
#

PROFILE_MOUNTS_HOST=""
PROFILE_UI="phosh"
PROFILE_KAOS_INIT="1"
PROFILE_MASK_EXTRA=""
PROFILE_PRESERVE_HAL="0"
PROFILE_INIT_PRIORITY=""
PROFILE_INSTALL_WRAPPERS="0"
PROFILE_HALIUM="0"
PROFILE_GFX="mesa"