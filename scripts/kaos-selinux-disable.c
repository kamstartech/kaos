/*
 * kaos-selinux-disable.so -- LD_PRELOAD shim that makes libselinux.so.1's
 * is_selinux_enabled() report "disabled" unconditionally.
 *
 * Ubuntu's dbus-daemon package is built with SELinux support (unlike
 * SailfishOS's own dbus, which has none -- confirmed via `strings` on both
 * binaries). is_selinux_enabled() only checks whether SELinux is compiled
 * into the kernel and /sys/fs/selinux is mounted and functional -- it does
 * NOT care whether the kernel is enforcing or permissive (that only affects
 * whether a denial blocks vs. just logs). This kernel actually defaults to
 * permissive on this ROM (system/core/init/selinux.cpp's
 * SelinuxSetEnforcement() is patched to force it there on every real
 * Android boot -- see CLAUDE.md's SELinux section, corrected 2026-09-24),
 * but SELinux is still genuinely compiled in and active. So once
 * /sys/fs/selinux is mounted (kaos-droid-hal-prepare.sh mounts it for
 * droid-hal-init's own genuine needs), dbus-daemon's SELinux integration
 * still activates (is_selinux_enabled() doesn't care that it's permissive)
 * and fails hard looking for /etc/selinux/targeted/contexts/dbus_contexts,
 * a policy file that doesn't exist on this plain Ubuntu rootfs. Even
 * shipping a stub file wouldn't really fix this: dbus-daemon would then
 * also call into the kernel to MAC-check every message against the REAL
 * loaded policy, which is Android's own sepolicy -- it has no notion of
 * Ubuntu's dbus object classes/contexts at all, so those checks would very
 * likely just fail closed instead. Confirmed live 2026-09-24: the user
 * session's dbus.service (started by `systemd --user` for uid 1000) hit
 * exactly this and exited 1 every time, which cascaded into phosh/
 * gnome-session never getting a working D-Bus session bus ("Could not
 * connect: Connection refused") and crash-looping. The system bus's own
 * dbus-daemon happened not to hit this on this boot only because it starts
 * before /sys/fs/selinux gets mounted -- a timing accident, not a real fix,
 * so this shim covers the user bus explicitly rather than relying on that
 * ordering.
 *
 * This makes Ubuntu's dbus-daemon behave exactly like Sailfish's own
 * non-SELinux build: skip the whole SELinux code path entirely, the same
 * way a normal desktop Ubuntu system (without SELinux compiled into its
 * kernel at all) would.
 */

int is_selinux_enabled(void)
{
    return 0;
}
