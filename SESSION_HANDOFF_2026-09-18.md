# Session Handoff — 2026-09-18 (Kernel EXIT_NAMESPACE fix / build-rootfs.sh overhaul / SailfishOS restore)

## 1. The core breakthrough: systemd-logind/journald EXIT_NAMESPACE, fully root-caused and fixed

**Symptom this session started from:** every hardened systemd unit (`systemd-logind.service`,
`systemd-journald.service`, and by extension anything depending on them — no user sessions, no
`/run/user/*`, no D-Bus user bus) crash-looped with `ExecMainStatus=226` (`EXIT_NAMESPACE`) on
this 4.9 kernel. This has been an open problem since a prior session's new-mount-API backport
(`open_tree`/`move_mount`/`fsopen`/`fsconfig`/`fsmount`/`fspick`, syscalls 428-433) didn't
actually fix it.

**Root cause, found by stracing the container's own systemd manager (PID 1 inside its own pid
namespace — safe, isolated, not the real host) across a live `systemd-logind.service` restart:**
two independent bugs in the *original* mount-API backport's `open_tree()` implementation in
`kernel/xiaomi/sdm845/fs/namespace.c`:

1. **`open_tree()` unconditionally required `OPEN_TREE_CLONE`.** Real upstream makes it optional
   — without it, `open_tree()` is just `open(path, O_PATH)` under a different name. systemd's
   `PrivateDevices=`/mount-rootfs setup calls it exactly that way (no `CLONE`), so every such
   call was rejected outright. Fixed: added the non-CLONE path using the same
   `dentry_open()`/`get_unused_fd_flags()`/`fd_install()` idiom `fs/nsfs.c`'s
   `open_related_ns()` already uses in this tree. Commit `a0bb8dc0c534`.

2. **`OPEN_TREE_CLOEXEC` was hardcoded to `0x02`.** Real upstream defines it as `O_CLOEXEC`
   itself (`0x80000` on this arch) so the flag passes straight through to
   `get_unused_fd_flags()`. Every real caller (glibc/systemd always pass real `O_CLOEXEC`) was
   silently rejected by `open_tree()`'s own flag-validation mask, which didn't recognize bit
   `0x80000` as valid. **This was the actual, complete fix** — found via a *raw* (undecoded)
   strace after the CLONE fix alone still didn't resolve it; a symbolically-decoded strace had
   been showing the wrong flags value (`0x102` instead of the real `0x80100`) the whole time.
   Commit `14b5abc69cf5`.

Also added in the same arc, real but not the actual blocker: `mount_setattr()` (syscall 442,
commit `fb1f33fb991d`, plus a `-Wvisibility` build-break fix `7988652057ec` — `struct mount_attr`
needs a file-scope forward declaration in `include/linux/syscalls.h`, same pattern already used
for `struct clone_args`/`sys_clone3`). This is still needed for `ProtectSystem=strict` etc., just
wasn't what was causing `EXIT_NAMESPACE` specifically.

**Verified end-to-end, live, over real SSH, on a fresh full Ubuntu (noble) build tonight:**
```
systemd-logind.service   Result=success  NRestarts=0  ExecMainStatus=0
systemd-journald.service Result=success  NRestarts=0  ExecMainStatus=0
```
Zero failed units related to this. `user@0.service`/`user@1000.service` both come up correctly
now (real PAM/logind sessions), which is what actually lets SSH-issued interactive sessions work
properly.

**Kernel commits, in order:** `fb1f33fb991d`, `7988652057ec`, `a0bb8dc0c534`, `14b5abc69cf5`
— all in `kernel/xiaomi/sdm845`.

---

## 2. The other half of the mystery: Ubuntu 26.04 (resolute) itself was rebooting the device

Separately from #1, starting the *ubuntu* namespace (any variant — full, UI-masked,
UI+networking-masked, doesn't matter) reliably rebooted the whole device ~12-14 seconds after
`kaos-service start`, every single time, all night — clean/voluntary `reboot()`, boot reason
plain `"reboot"`, no panic, no watchdog signature, nothing in dmesg. This looked at first like it
might be connected to #1 (both surfaced the same night), but it wasn't.

**Bisection process** (documented for anyone re-deriving this):
- Masking `phosh.service`/`kaos-ui-stream.service` (display stack) on a live ubuntu install →
  still rebooted.
- Also masking the three `kaos-wlan`/`kaos-dhcpcd-wlan`/`kaos-wifi-route` units (networking) →
  still rebooted. Ruled out display and networking as the trigger.
- Built a **genuinely bare** kali rootfs (`--bare`, new `build-rootfs.sh` feature — see §3) from
  a completely different codebase (`kali-rolling`/Debian) → ran stable for 45+ minutes. Ruled out
  `kaos-service`/kernel/general namespace mechanics as the trigger.
- Built a genuinely bare **ubuntu** rootfs on the *same* `resolute` (26.04) codename as all the
  broken tests → never actually got to test this combination in isolation (jumped straight to
  reverting the codename on a hunch).
- Reverted `build-rootfs.sh`'s ubuntu case from `resolute`/26.04 back to `noble`/24.04 (the
  version it was on before an earlier session bumped it for greetd/phrog support — which turned
  out to buy nothing anyway, since `kaos-service` wires `phosh.service` directly onto
  `graphical.target` and never uses greetd at all).
- Rebuilt **both** bare and full ubuntu on `noble` → both stable, no reboot, confirmed multiple
  times including a full build with the complete UI/Mesa/networking stack.

**Conclusion: `resolute`/26.04 itself was the trigger**, not scope (bare vs full), not
`kaos-service`, not the kernel. Root cause of *why* resolute specifically breaks something was
never pinned down (would need another bisection pass, e.g. diffing package/systemd versions
between resolute and noble) — but reverting the codename is a complete, verified fix in practice.

**`build-rootfs.sh`'s `ubuntu` case is now back on `noble`/24.04, `BUILD_IMAGE="ubuntu:24.04"`.**

---

## 3. `build-rootfs.sh` overhaul

Substantial changes to `hybris/kaos-configs/build-rootfs.sh` tonight:

### Interactive picker
Running `./build-rootfs.sh` with **no arguments** now prompts: distro (ubuntu/debian/kali), then
GPU/display stack in or out, then networking in or out, shows a summary, asks to confirm. CLI
flags still work for scripting: `--distro <name> [--no-ui] [--no-net] [--bare]`.

### `WITH_UI`/`WITH_NET` split (replaces the old single `--bare`)
- `WITH_UI` (default on): Mesa/GPU (KGSL), `libseat-fake`, `ui-capture`, `phosh-core`.
- `WITH_NET` (default on): dhcpcd/wpasupplicant/iproute2, the three `kaos-wifi-*` units, static-DNS
  resolved drop-in.
- `--bare` = shorthand for both off. Output filename reflects the combination:
  `kaos-<distro>-rootfs.tar.xz` (full), `-no-ui-`, `-no-net-`, `-bare-`.
- This maps directly onto how the reboot mystery was actually bisected live tonight (§2) — built
  specifically so that kind of isolation is reproducible from a clean build instead of
  live-patching units into an already-built rootfs by hand.

### Now installed **unconditionally** (every build, bare or full — all found live tonight):
- **`xz-utils`/`zstd`** — a bare debootstrap rootfs has no way to decompress `.deb` control/data
  members once it needs to install anything after the fact (Kali ships zstd-compressed `.deb`s
  by default). This is a genuine chicken-and-egg trap if hit live post-deploy: `zstd`'s own
  package is itself zstd-compressed, so `apt` can't install its way out without a working `zstd`
  already present. (The live `kali-bare` instance that hit this got manually bootstrapped by
  extracting binaries from `.deb`s using the host's own `ar`/`tar`/`zstd` and pushing them in
  directly — not needed anymore for fresh builds.)
- **`openssh-server`**, `ssh.service` explicitly enabled (no live systemd during build for the
  package's own postinst to do it), `PermitRootLogin yes` via a drop-in
  (`/etc/ssh/sshd_config.d/10-kaos-root-login.conf`). SSH keys are deliberately **not** baked in
  — that's a per-deployment credential, provisioned at deploy time (see §5).
- **`gdb`/`strace`** — added at the very end of the session for the still-open D-Bus segfault
  investigation (§6).

### `defaultuser` (uid 1000), unconditional, replaces the old UI-only `phosh` account
Moved out of the `WITH_UI` gate entirely and renamed from `phosh` to `defaultuser` — matching
SailfishOS's own convention, so every build (bare or full) gets a consistent, known login account
instead of bare builds having *no* non-root account at all. Same uid (1000) still satisfies
`phosh.service`'s hardcoded `User=1000` requirement for `WITH_UI=1` builds — no separate account
needed. No password set (matches `sailfish`'s pattern); access is key-based only. Confirmed
`kaos-service` never references the username `phosh` anywhere (only the package/binary/path name
`phosh`), so this rename is safe.

### A real, separate bug found and fixed along the way: `kaos-starter exec`
`kaos-starter`'s `exec`/`login` commands hardcoded `/system/bin/chroot` (an **Android** path) as
the final step after `nsenter -m` joins the container's mount namespace. Once `-m` actually takes
effect, `/system/bin` doesn't exist in that view at all (it's not FHS) — so this failed with
`can't execute '/system/bin/chroot': No such file or directory`. Worse: on distros where
`nsenter -m` *failed* to take effect for some unrelated reason (e.g. a stale target PID from PID
reuse), `/system/bin/chroot` still resolves fine on Android's own root, so the command would
silently "succeed" while actually running against **Android's filesystem instead of the
container's**, with no visible error — this is almost certainly why several `kaos-starter exec`
checks earlier this project's history looked like they worked but weren't actually testing
anything. Fixed: use `/sbin/chroot` (present on every debootstrap-based rootfs this project
ships — ubuntu/debian/kali, bare or full) instead. `kaos/scripts/kaos-starter`, not yet a git
commit (script lives outside a git repo at that path — edited in place, pushed to
`/system/bin/kaos-starter` on-device).

---

## 4. `distros.conf` / cold-boot ROM selector: real, verified findings

- **`kaos-chroot-install` is *not* used for anything deployed tonight.** All of `kali-bare`,
  `ubuntu-bare`, the full `ubuntu` rebuild, and `sailfish` were deployed manually (`adb push` +
  direct `tar`/`busybox_kaos xzcat|tar` extraction), per explicit instruction this session — the
  installer was avoided after an earlier-session incident where it collapsed to a dangerous bare
  path for an unregistered distro name (already guarded against — see `distros-lib.sh`'s
  `resolve_distro()` fail-fast checks from a prior session — but the instruction tonight was to
  just not exercise that code path at all for these deploys).
- **The cold-boot ROM selector menu requires *both* `/mnt/vendor/persist/trampoline_primary` to
  exist *and* `status=installed` (exactly that string) in the relevant `distros.conf` section —
  not just one or the other.** Found by reading `system/extras/multirom/trampoline/trampoline.c`
  directly: `run_init_second_stage()`'s Step 2 does `stat(BOOT_FLAG_FILE)`
  (`BOOT_FLAG_FILE` = `/mnt/vendor/persist/trampoline_primary`) and fast-paths straight to
  Android if it's missing — contradicting `CLAUDE.md`'s old claim that the menu "needs no enable
  flag." Separately, both `trampoline.c`'s own `lookup_distro_rootfs()` and
  `multirom_load_hybridos_distros()` in `multirom.c` only list a distro if its `status=` field is
  exactly `installed` — `status=available` is silently excluded from the menu entirely, even with
  the flag present. **`CLAUDE.md`'s Cold-Boot ROM Selector section has been corrected in place**
  to describe this accurately, with the actual source snippet quoted.
- Confirmed live: the boot flag was already present on-device from an earlier, unrelated session
  (dated 2026-04-14) — the actual blocker for "no distro shows at boot" was purely the missing
  `status=installed`. Fixed and reboot-tested same night: SailfishOS booted successfully from the
  cold-boot selector after the correction.
- `kaos-chroot-install`'s `do_restore()` **never writes `status=installed` on success** — a real,
  separate gap in the installer itself (not touched tonight, since the installer wasn't used —
  worth fixing at the source if `kaos-chroot-install` starts getting used again for real
  deployments).

---

## 5. SailfishOS: real, deployable release found and restored

Found a **complete, working SailfishOS 5.0.0.76 "Tampella" release** sitting at
`SailfishOScommunity-release-5.0.0.76-perseus/sailfishos-perseus-release-5.0.0.76.zip` — not
previously known/referenced from `CLAUDE.md` or any doc that was checked first. Contains a
recovery-flashable zip (`hybris-boot.img` + `sailfishos-perseus-release-5.0.0.76.tar.bz2`, the
actual rootfs).

The real deploy procedure is documented at `hybris/droid-configs/SAILFISHOS_DEPLOY.md` — also not
linked from `CLAUDE.md`, took several turns to even find. It's a solid, complete doc; just
orphaned. One correction made while following it: its `sed` command sets
`Storage=automatic` for journald, which **isn't a real systemd value** (valid values are
`volatile`/`persistent`/`auto`/`none`) — used `persistent` instead, consistent with the fix
applied to the other distros this session.

Deployed twice tonight (once, then re-deployed after an unrelated app-side deletion accident —
see below) using the documented manual `adb push` + `tar -xjf` + SSH-key-install steps. **Booted
successfully from the cold-boot ROM selector**, live-verified, once the `status=installed`
fix (§4) was in place.

**Real bug found and fixed in the Kaos Manager Android app while investigating an accidental
deletion:** `SetupManager.java`'s `delete()` reads a shared mutable `currentDistro` field
(set via `setCurrentDistro()`) inside a spawned background thread rather than a value captured
synchronously at call time — investigated as a possible race but the `isRunning` guard in
`DeploymentService` turned out to correctly serialize it, and `DistroManager.getDistro()` is a
clean map lookup. `sailfish`'s deletion turned out to be unrelated to this delete flow entirely
(it's never in `distros.conf`, so the app has no way to reference it) — likely just broader
fallout from whatever command was actually run. Not a proven bug, but worth another look if a
similar cross-distro deletion happens again.

---

## 6. Open issue, not yet root-caused: GLib/GDBus segfaults on system-bus connection

Found live tonight on the full `ubuntu`/noble rebuild, after fixing WiFi: `apt-get update`'s
`APT::Update::Post-Invoke-Success` hooks (`50appstream`'s `appstreamcli refresh`, `20packagekit`'s
`gdbus call ... org.freedesktop.PackageKit ...`) both **segfault**. Isolated further: even the
most trivial possible D-Bus call —
`gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames`
— segfaults immediately. This is the same underlying crash as `polkitd` itself failing
(`ExecMainStatus`/SEGV, confirmed via `systemctl status` and reproduced directly by running
`/usr/lib/polkit-1/polkitd --no-debug` manually — instant `exit 139`).

**Working hypothesis, not yet confirmed:** GLib's GDBus implementation probes `SO_PEERPIDFD`
(pidfd-based peer credential passing over Unix sockets) when connecting to the system bus. This
kernel's backports don't provide it — `systemd-logind`'s own foreground strace (captured earlier
this session, in a completely different context) showed the *identical* syscall failing cleanly:
```
getsockopt(12, SOL_SOCKET, SO_PEERPIDFD, 0x7fcf928510, [4]) = -1 ENOPROTOOPT (Protocol not available)
```
`systemd` handles that `ENOPROTOOPT` gracefully and falls back. The hypothesis is that GLib/GDBus
does the same probe but doesn't handle the failure gracefully, crashing instead. **Not confirmed**
— no `gdb`/`strace` were available in the running instance to get a real backtrace before the
session ended.

**Next step, ready to go:** `gdb`/`strace` are now baked into `build-rootfs.sh` unconditionally
(§3) — the user is building a fresh rootfs with them included. Once deployed, reproduce the crash
under `strace -f` or `gdb` and confirm (or rule out) the `SO_PEERPIDFD` theory with an actual
backtrace instead of guessing from the outside.

---

## 7. Current on-device state (as of session end)

`distros.conf` (`/data/.stowaway/distros.conf`):
```
[ubuntu]      status=installed   -- full build, noble/24.04, currently deployed
[kali]        status=available   -- registered, directory not present
[sailfish]    status=installed   -- SailfishOS 5.0.0.76, deployed and boot-tested
[kali-bare]   status=installed   -- bare build, debian(kali-rolling)-based
```
`active` symlink → `ubuntu`.

- `ubuntu` (full, noble): last confirmed `Active`, SSH-reachable, `logind`/`journald` clean, WiFi
  connected (`levelbuild Liquid 5G`, credentials pulled from Android's own
  `WifiConfigStore.xml` and added to `/etc/wpa_supplicant/wpa_supplicant.conf`), `polkitd`/GDBus
  segfault still present and unresolved (§6).
- `kali-bare`, `sailfish`: deployed, `defaultuser`/`sshd` present, not the currently-active
  namespace.
- A fresh `ubuntu-bare` was built and tested during the bisection (§2) but was later removed
  (superseded by the full-build confirmation) — not currently on-device.
- SSH access to all deployed distros: key-based only (`~/.ssh/id_ed25519.pub` pushed to both
  `/root/.ssh/authorized_keys` and `/home/defaultuser/.ssh/authorized_keys` per distro at deploy
  time — not baked into the build). Standard access pattern:
  `adb forward tcp:2223 tcp:22` then `ssh -p 2223 root@127.0.0.1`.

## 8. Suggested next steps

1. Get a real backtrace on the `polkitd`/GDBus segfault (§6) once the `gdb`/`strace`-equipped
   rootfs is built — this is the most concrete open thread.
2. If confirmed as `SO_PEERPIDFD`-related, decide whether to backport `SO_PEERPIDFD` support
   properly into the kernel (same style as tonight's `open_tree` work) or find another mitigation
   (e.g. an older/patched GLib).
3. Root-cause *why* `resolute`/26.04 specifically broke things (§2) if it's ever worth knowing —
   not blocking, since `noble` is a complete, verified fix, but the actual mechanism was never
   found.
4. `kaos-chroot-install do_restore()` never sets `status=installed` on success (§4) — worth fixing
   at the source if the installer gets used for real deployments again.
5. `kaos-starter`'s `/sbin/chroot` fix (§3) needs pushing to `/system/bin/kaos-starter` on any
   fresh flash/device — currently only live-patched on this device, not yet folded into a ROM
   build.
