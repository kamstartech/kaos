# Session Handoff — 2026-09-19 (clone3/TLS SIGSEGV fix / namespace teardown watchdog)

Title: Kernel copy_thread_tls/HAVE_COPY_THREAD_TLS root cause + kaos-service teardown watchdog + full WiFi provisioning.

## 1. The D-Bus crash cluster was clone3 + CLONE_SETTLS losing the TLS register — NOT SO_PEERPIDFD

Symptom everyone started from this session: the freshly-deployed full ubuntu (`noble`) instance had
a cluster of 15 failed services. `polkitd`, everything gdbus-based, and even `gdb`/`python`
threading crashed with `SIGSEGV`/`exit 139`. §6 of yesterday's handoff guessed `SO_PEERPIDFD` —
**that theory is now conclusively disproven**; the real cause was a kernel bug in the `clone3`
backport.

**Backtrace method that cracked it** (no need for kernel sources to be mounted — worked entirely
against the deployed instance): `strace -f` around a single `threading.Thread().start()` on
Python showed the child (a fresh `clone3(CLONE_SETTLS|CLONE_THREAD|...)` clone) dying with
`SIGSEGV` at addr NULL on its *first* instruction — zero syscalls executed, meaning the failure is
100% in the utility/arch thread-teardown, not in anything the thread does. Same observed for `gdb`
itself and `gdbus call`.

**Why TLS was getting lost:** `clone3` returns the child's regs from a `copy_thread` whose
signature only match-sets regs[0..3]. This is fine when the parent's syscall shim hands the
child's register state through (classic `clone`/`fork`), but `clone3` sets `CLONE_SETTLS` via a
separate `tls` argument, not through a register. In this tree, the new-syscall glue
(`kernel/xiaomi/sdm845`, sched.h `INIT_THREADS`/`copy_thread_tls` fallback, more on the mechanism
below) didn't pre-wire `HAVE_COPY_THREAD_TLS`; the sched.h fallback in `kernel/fork.c`'s
`copy_thread_tls()` shim then silently dropped `tls` entirely, and 4.9 arm64's `copy_thread`
reads TLS from `regs[3]` — which for a `clone3` child is just whatever garbage the parent's
syscall-saved regs happen to contain (classic-clone writes the `tls` pointer into regs[3]'s slot
before starting the child; `clone3` doesn't). Result: every `CLONE_SETTLS` child starts with a
bogus `tpidr_el0`, touching TLS (stack canaries, errno, pthread keys) segfaults
deterministically.

**Fix (uncommitted working tree, kernel/xiaomi/sdm845):**
- `arch/arm64/kernel/process.c`: `copy_thread()` → `copy_thread_tls(..., unsigned long tls)` and
  use `tls` when `CLONE_SETTLS` is set.
- `arch/arm64/Kconfig`: `select HAVE_COPY_THREAD_TLS`.
- `init/Kconfig`: add `config HAVE_COPY_THREAD_TLS` (guarded/traditional arm64 config).

Verified end-to-end live after user rebuilt and flashed the boot image:
```
python3 -c "import threading; [threading.Thread(target=lambda: print('ok')).start() for _ in range(8)]"
gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ListNames     # rc=0
```
Everything in the crash cluster active: polkitd, NetworkManager, udisks2d, phosh, kaos-ui-stream,
accounts-daemon, switcheroo-control, rtkit-daemon, gnome-remote-desktop. `systemctl reset-failed`
→ **0 failed**. Only still-failing after that: `tpm-udev` (missing tpm hardware — benign) and
`NetworkManager-wait-online` (no default route at check time — benign).

## 2. The other pending thread from yesterday solved too: `strace`/`gdb` got their rebuild

The user rebuilt the rootfs with the always-installed `strace`/`gdb` from §3 of yesterday's notes
(`droid-local-repo/perseus/kaos/kaos-ubuntu-rootfs.tar.xz`, `2026-09-18 20:39`, contains both) and
redeployed manually: backup `distros.conf` + key authorization (`/data/local/tmp/kaos-deploy-backup/`),
wipe `/data/.stowaway/ubuntu`, `busybox_kaos xzcat | tar -C /data/.stowaway/ubuntu -x`, restore keys,
start. This directly enabled the §1 backtrace work (and confirmed the `resolute`-reverted `noble`
build is what's running — reboot behavior moot).

## 3. WiFi: all 12 networks provisioned, not just the one

Previously WiFi was manually single-network. This session pulled **every** network from Android's
`/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml` and wrote configured entries into
`/etc/wpa_supplicant/wpa_supplicant.conf` inside the distro rootfs (device file already mounted at
chroot-deploy): **12 networks total** — PSK-protected home (`Kamstarlink`/`L3v3l-levelbuild`),
plus open APs (`Galaxy A73 5G 6735`, `LIQUID-GUEST`, `LIQUID-WIFI`, `PH by Marriott Lusaka-Hotspot`,
`STARLINK`, `Sanctuary Guest`, `4f86d502a6`). One real bug fixed en route: escaping the PSK for
`Kamstarlink` — first pass left a stray `\`, corrected the PSK to `K@m$T4rT3ch` and the entry
parses/connects. Connected, online, `192.168.1.38` on `Kamstarlink`.

## 4. Namespace teardown watchdog + `reap` recovery command (kaos-service)

The breakage this resulted in: `poweroff` **inside** the container did a clean container shutdown
(log showed 11:07 clean stop, then an 11:28 start whose PID 1 exited without teardown) but left
Android-side cruft behind — nothing monitors that side. Orphaned `kaos-touch-input`
(host 7162) + `kaos-display-bridge` (host 7163) kept running with their PID files stale, and a
stack of stale mounts survived at `/data/.stowaway/ubuntu/{system,vendor,sdcard,dev/dri,dev/socket,mnt/android-dev-socket,mnt/android-dev-snd}`
plus `/data/rootfs` (sda22). Root cause of the stale-mount path: those binds were made when
`CHROOT` resolved to `/data/.stowaway/ubuntu` (older config); current `distros.conf` (`path=ubuntu`)
resolves `CHROOT=/data/rootfs`, `REAL_CHROOT=/data/.stowaway/ubuntu`, and the old fixed-string
unmount list missed both. Fixed in `kaos/scripts/kaos-service` (hot-deployed to
`/system/bin/kaos-service`):

- **`teardown_all()`** — shared teardown used by explicit `stop`, the new watchdog, and the new
  `reap`. Kills `kaos-touch-input`/`kaos-display-bridge` from their PID files, removes all
  `kaos-*.pid` files, then unmounts **every** mount under **both** `$CHROOT` and `$REAL_CHROOT`
  (dynamic `/proc/mounts` parse, deepest-first) — catches binds from old configs. Base `$CHROOT`
  unmount is now unconditional + lazy (`runcon u:r:su:s0 umount -l "$CHROOT"`) because
  `mountpoint -q` proved unreliable in the bridge's context (returned "not a mountpoint" while
  `/proc/mounts` still showed `/data/rootfs` atop sda22).
- **Watchdog** — after a successful `start`, a fully-detached subshell
  (`(... ) < /dev/null > /dev/null 2>&1 &` — without the fd redirection the spawned watchdog's pipe
  keeps `kaos-service start`'s bridge session open indefinitely, hang on caller; learned live)
  polls `/proc/$MAIN_PID` every 5s; when the `unshare -f` wrapper exits (container PID 1 gone —
  poweroff, crash, oom, SIGKILL) it verifies `$(cat $PID_FILE) == $MAIN_PID` (race/PID-reuse guard
  against concurrent explicit stop) then runs `teardown_all` + `do_detect`. Logs
  `"namespace PID 1 exited; auto-teardown"`.
- **`reap` subcommand** — recovery for the exact "stuck" state above: `kill_namespace_tree` +
  `teardown_all`, tolerant of everything already being gone. `kaos-service --distro <d> reap`.
- **`kill_namespace_tree()`** extracted from `stop` (via `/proc/$wrapper/task/$wrapper/children`
  → kill namespace PID 1 first, then wrapper — no pgrep, mirrors documented behavior).
- `kaos/scripts/distros-lib.sh` `distro_umount()` raised to `runcon u:r:su:s0 umount` (a bare
  toolbox umount can't drop a real device mount like the sda22 `/data/rootfs`).

Verified end-to-end live: `reap` cleared the stuck state (proc + PID files + mounts all zeroed);
then a full fresh cycle — `start` (returns immediately, no hang), SSH in, `poweroff` from inside
the container, **wait**: watchdog fired (`12:09:41 ... auto-teardown`), no namespace processes, no
PID files, **zero stale mounts** (both `/data/.stowaway/ubuntu/*` and `/data/rootfs` gone).

Deploy mechanics note: `/system` is read-only at boot on this device (system-as-root, `/ = dm-1
ro`). Standard AGENTS.md "push straight to /system/bin" silently no-ops (Read-only file system). To
hot-deploy: `adb push` to `/data/local/tmp/`, then over the bridge
`mount -o remount,rw /` + `cp` + `chmod 755`. The device's `/` is currently left `rw`; it reverts
to `ro` at next reboot.

## 5. Current on-device state (as of session end)

- Kernel: `4.9.337-perf-g14b5abc69cf5-dirty` (clone3 TLS fix flashed). Fix **uncommitted** in
  `kernel/xiaomi/sdm845` (`arch/arm64/kernel/process.c`, `arch/arm64/Kconfig`, `init/Kconfig`).
- `distros.conf`: `ubuntu` active (full `noble`, rebuild from `2026-09-18 20:39` rootfs),
  `sailfish` installed, `kali-bare` installed, others available. `active` symlink → `ubuntu`.
- Namespace: currently **stopped/clean** (final poweroff test left it down; `reap`-clean, no stale
  mounts, no orphan daemons). `/data/rootfs` unmounted.
- WiFi: 12 networks configured in distro wpa_supplicant; last connect `Kamstarlink`,
  `192.168.1.38`.
- `kaos-service` + `distros-lib.sh` hot-deployed with watchdog/reap/`distro_umount` fixes.
- SSH pattern unchanged: `adb forward tcp:2223 tcp:22`; key-based `root@127.0.0.1` (`id_ed25519.pub`).

## 6. Suggested next steps

1. **Commit + bake the clone3 TLS fix** (§1). Verify the three files are complete
   (`process.c`, `arch/arm64/Kconfig`, `init/Kconfig`) and craft a message in the repo's style.
2. **Fold the watchdog/reap work into the ROM build** (not just hot-deployed): `kaos-service` +
   `distros-lib.sh` changes live in `/system/bin/` on-device; a fresh flash would lose them.
3. Consider wiring `kaos-starter`'s `/sbin/chroot` fix (§3 of yesterday) into this push too, so the
   next full build carries all three.
4. Leave `remount,rw /` as a documented dev convenience or normalize `/` back to `ro` after next
   deploy (one-liner either way — noted in §4).
5. If the container poweroff path is exercised again, the watchdog should now keep the host clean
   without a `reap`; anything that re-adds fixed-string unmount lists should be viewed with
   suspicion (dynamic `/proc/mounts` sweep is strictly more robust).

## 7. Kernel cleanup (post-handoff)

All 12 pre-existing uncommitted kernel files (`binder`, `cgroup`, `drm`, `fts`,
`fs/proc`, `perseus.config`) were reviewed. Findings + fixes, all on
`lineage-22.2-nethunter` → `kamstartech`:

- `d15a900ca767` binder: cmd 'b' **16 is `BINDER_ENABLE_ONEWAY_SPAM_DETECTION` in this
  AOSP ABI** (verified across every binder.h in tree), NOT thread-pool-exhausted (that's
  'b' 18 in newer AOSP and is not shipped). Handler renamed; no-op ack is correct
  (4.9 lacks the feature). Also dropped the stale `CONFIG_PROVE_LOCKING=y` block
  (dated 2026-09-15 "TEMPORARY", superseded by the proc backport).
- `bb80b4dbc93c` proc mount_ns() backport — s_fs_info ref balance verified 1:1
  incl. error path.
- `e459a6b2d7a2` drm crtc_id in vblank events + `DRM_CAP_CRTC_IN_VBLANK_EVENT` (0x12).
- `c0d3c424f79e` fts: drop redundant double `ABS_MT_TRACKING_ID -1` (core already
  emits it on `input_mt_report_slot_state(...,0)`).

**Intentionally left UNCOMMITTED (do not touch):** `kernel/cgroup.c` +
`include/linux/cgroup-defs.h` — v2 `nsdelegate`/`memory_recursiveprot` accepted but
no-op on 4.9; remount silently drops unlisted flags; ordered only for next visit.

**Junk, never commit:** `firebase-debug.log`, `out/` (add to `.gitignore`).

These are source/logic fixes only — the flashed kernel still behaves identically for
binder (name-only change); PROVE_LOCKING removal only affects the next rebuild.