# Session Handoff — 2026-09-23 (switch_root WLAN bring-up, cold-boot graphics diagnosis, overlay-vs-coldboot resource fight)

Subtitle: Pushed the ubuntu switch_root cold-boot bring-up further than ever before (reached
`icnss: QMI Server Connected`), root-caused why cold-boot's UI never appears (GPU swapchain/buffer
failure, matching the already-known missing shim gap from 2026-09-21), then found and fixed a
**self-inflicted regression**: the cold-boot-only systemd units added this session were also firing
inside `kaos-service`'s overlay/namespace mode (same shared rootfs), causing a second `droid-hal-init`
to fight Android's already-running one — which is what was killing the overlay session. Also hit and
learned around a `journalctl`-on-Android-shell dead end.

**Read this before touching `kaos/scripts/kaos-service`,
`hybris/droid-configs/sparse/kaos.init` (+ `kaos/apps/phosh-app/res/raw/droid_dm_setup.sh` mirror),
`hybris/droid-configs/sparse/usr/local/sbin/kaos-droid-hal-init.sh`, or any of the 8 systemd unit files
under `hybris/droid-configs/sparse/usr/lib/systemd/system/`.** The overlay/cold-boot marker mechanism
(§3) is load-bearing across all of them.

No git at the repo root; sub-repos are git. State below is direct file state + on-device verification
as of session end.

---

## 1. Ubuntu switch_root WLAN bring-up — furthest point reached yet

Continuing from 2026-09-21's "systemd dies instantly" baseline, this session fixed a long chain of
blockers in the cold-boot path, each confirmed live via clear-logs → reboot → pull-journal cycles:

1. **`droid_hal_init` missing from `PRODUCT_PACKAGES`** (`kaos/kaos.mk`) — built but never packaged,
   matching the file's pre-existing pattern for `libhwc2_compat_layer`/`libminisf`. Fixed, rebuilt,
   reflashed by user; confirmed live at `/system/bin/droid-hal-init`.
2. **`/system/bin` near-empty inside the switch-rooted tree** — `mount /run/droid/system /system`
   mounts the dm-linear partition's own root, whose top-level `/bin` is a stub; real content is nested
   at `<partition>/system/bin`. Fixed by porting SailfishOS's real `system_root.mount` (raw partition →
   `/system_root`) + `system.mount` (bind `/system_root/system` → `/system`) pair, plus
   `vendor.mount`/`vendor-firmware_mnt.mount`/`system_ext.mount`/`product.mount`/`odm.mount` —
   byte-identical copies of SailfishOS's own units, all `WantedBy=local-fs.target`.
3. **`droid-hal-init`'s self-exec** (`execv("/sbin/droid-hal-init") failed: No such file`) — fixed with
   `ln -sf /system/bin/droid-hal-init /sbin/droid-hal-init` (merged-usr symlink target).
4. **`/linkerconfig` missing** — `mkdir -p /linkerconfig /debug_ramdisk /second_stage_resources`.
5. **selinuxfs never mounted** → every service start logged `security_setenforce(0) failed` /
   `Could not get process context`. Fixed: `mount -t selinuxfs selinuxfs /sys/fs/selinux`.
6. **Real FBE-encrypted userdata mount** → every `/data/*` mkdir failed `Required key not available`.
   Fixed by bind-mounting a plain, persistent `/android_data` directory onto `/data` *before*
   `droid-hal-init` runs, exploiting `MountRealDataIfNeeded()`'s (`system/core/init/init.cpp:959`)
   own `IsMountPointFor` early-out.
7. **keystore2/keymaster-4-0/gatekeeper-1-0 SIGABRT crash-loop.** User explicitly rejected the first
   instinct (disable the crashing services) and asked to root-cause instead. Real tombstone
   (`/data/.stowaway/ubuntu/android_data/tombstones/tombstone_04`) showed
   `Could not register service for Keymaster 4.0 (-2147483648)`; log showed
   `hwservicemanager not found` — `hwservicemanager.rc` lives on `/system_ext`, which was never
   mounted. Fixed by the `system_ext.mount`/`product.mount`/`odm.mount` units in item 2. Confirmed
   live: 0 SIGABRTs.
8. **Silent ~4-minute stall after `art_boot`.** Traced through `system/core/rootdir/init.rc` to
   `wait_for_prop odsign.key.done 1` immediately after a `start odsign` that a prior round had already
   stripped without removing the paired wait — a self-inflicted deadlock (Android init's command queue
   is single-threaded and blocks on `wait_for_prop` forever if the property is never set). Fixed by
   also stripping the `wait_for_prop odsign\.key\.done 1$` line in `patch_rc_init_hybris()`
   (`kaos-droid-hal-init.sh`). This was the key unblock — `qrtr-ns`/`pd_mapper`/`vendor.per_mgr`/
   `vendor.wifi_hal_legacy` all started afterward, reaching **`icnss: QMI Server Connected`** — the
   furthest point ever reached in this investigation.

**Still open:** `wlan0` never appeared as a real netdev in any test run so far, even past
`icnss: QMI Server Connected`. Real Android's own gap between QMI-Connected and `WLAN FW is ready` is
only ~3s, so this may be pure timing (tests so far only waited ~1 min before rebooting back) — or there
may be one more real blocker. **Needs one more cold-boot test with more patience past that point.**

All the `kaos-droid-hal-init.sh` mount-diagnostic/APEX-fallback/rc-patch logic from this thread lives in
`hybris/droid-configs/sparse/usr/local/sbin/kaos-droid-hal-init.sh`, wired in by
`hybris/droid-configs/sparse/usr/lib/systemd/system/kaos-droid-hal-init.service`
(`WantedBy=graphical.target`, `Before=graphical.target`, `After=local-fs.target`).

---

## 2. Cold-boot graphics — root cause confirmed, user's socket hypothesis ruled out

User asked to check UI status and offered a hypothesis: *"in overlay mode the display connects to an
already running sf but in this setup there is no service for our socket to connect to."* Tested against
real captured phoc output (had to first add explicit `StandardOutput=append:…`/`StandardError=append:…`
to the generated `phosh.service.d/kaos.conf` — phoc's crash was producing zero journal output
otherwise). Evidence did **not** match the socket hypothesis (no socket-refused/missing-service lines
anywhere); the actual, repeated failure is:

```
[types/output/swapchain.c:109] Swapchain for output 'DSI-1'/'Virtual-1' failed test
```

This is a genuine GPU buffer allocation/import (KGSL↔DRM interop) failure — matching the 2026-09-21
handoff's already-identified gap: `kgsl_drm_shim.so`/`kaos-drm-redirect.so` are referenced via
`LD_PRELOAD` in `kaos-service` (L719/L795) but **do not exist anywhere** — no source, no binary, no git
history. Separately, phoc also logs `Failed to open device: '/dev/input/eventN': Permission denied`
for every input device — a simpler, unrelated permission gap, not yet fixed.

**How overlay's graphics avoid this entirely** (confirmed from source, not guessed):
`WLR_BACKENDS=headless,libinput`, `WLR_RENDERER=vulkan` — phoc renders **headlessly**, no DRM/KMS
scanout at all. `kaos-ui-stream.service` (ffmpeg) captures the headless output and relays frames over
`/dev/socket/kaos_ui.sock` to `kaos-display-bridge`, which presents them through Android's already-live
display stack. Cold boot's `WLR_BACKENDS=drm,libinput` + `WLR_RENDERER=gles2` +
`MESA_LOADER_DRIVER_OVERRIDE=zink` path has no equivalent "already-running compositor" to lean on —
this is architecturally why cold boot needs a real KGSL↔DRM bridge (or a hwcomposer path, see the
2026-09-21 handoff §5) and overlay doesn't.

**Not decided yet** (user is still in an understanding-building phase, explicitly deferred a choice
between: write `kgsl_drm_shim.so` from scratch / adopt Halium+hwcomposer per the 2026-09-21 direction /
a simpler headless-render-and-locally-present bridge). Do not pick a direction without the user.

---

## 3. Overlay-vs-cold-boot resource fight — found, root-caused, fixed, verified live

User later said: *"With the changes we have already made can you check the state of the device, I have
started ubuntu in overlay mode."* The overlay session was found **dead** (`kaos-service status` →
`Inactive`, none of `phoc`/`phosh`/`unshare_kaos`'s children present) despite having just been started.

**Root cause:** the cold-boot-only systemd units added in §1 (`kaos-droid-hal-init.service` +
`system_root.mount`/`system.mount`/`vendor.mount`/`vendor-firmware_mnt.mount`/`system_ext.mount`/
`product.mount`/`odm.mount`) are installed **rootfs-wide**
(`WantedBy=local-fs.target`/`graphical.target` symlinks inside the shared ubuntu rootfs), and that same
rootfs is what `kaos-service`'s overlay/namespace mode also boots — via `ns-entry.sh` exec'ing the
rootfs's own real `/sbin/init` (systemd). Overlay's systemd therefore *also* ran these units, which:
- remount raw dm-linear partitions Android already had mounted (redundant at best), and
- worse, `kaos-droid-hal-init.service` execs a **second `droid-hal-init`** that fights the real,
  already-running Android one for the same `hwservicemanager`/HIDL registrations.

User's own diagnosis nailed it: *"Looks like after our changes overlay fights for resources."*

**Fix (mode marker file, `/etc/kaos-overlay-active`):**
- `kaos-service`'s generated `ns-entry.sh` heredoc now does `touch /etc/kaos-overlay-active` right
  before `exec $INIT` (real systemd), regardless of which init candidate was actually selected.
- `hybris/droid-configs/sparse/kaos.init` (+ its `droid_dm_setup.sh` mirror) — switch_root-only, never
  runs for overlay — now does `rm -f /etc/kaos-overlay-active` at the very start of `=== START ===`, so
  a stale marker from a prior overlay session against the same rootfs can never mask cold boot's own
  units.
- All 8 cold-boot-only unit files gained `ConditionPathExists=!/etc/kaos-overlay-active`
  (`kaos-droid-hal-init.service` + the 7 `.mount` units listed above).

**Verified live:** pushed all 8 rootfs files + the updated `/system/bin/kaos-service` binary (had to
`adb remount` first — `/system` is `ro`), restarted `kaos-bridge` (had been left stopped mid-flow from
an earlier `&&`-chain failure — watch for this), then started ubuntu overlay fresh. Result: session
stayed `Active`, and — unlike the broken run — `phoc`, `phosh`, `phosh-osk-stub`, and `kaos-ui-capture`
were all actually running. Marker file confirmed present at
`/data/.stowaway/ubuntu/etc/kaos-overlay-active` (`touch`ed at overlay start) and confirmed **absent**
after a subsequent real switch_root cold boot (removed by `kaos.init`, as designed).

**Not yet folded into a real rebuild** — user owns builds; these are live-pushed to the device only.
Needs `mka bacon`/reflash (or at minimum a rootfs rebuild carrying the two systemd-unit-affecting
files) to persist.

`wlan0` was up but `NO-CARRIER`/state `DOWN` during the working overlay verification — a separate,
pre-existing WiFi-association question (no AP configured/connected in that container), not part of this
bug.

---

## 4. `journalctl` on Android shell — dead end, use chroot or delete files directly

Learned the hard way this session, twice:

- **`journalctl` is not directly executable from a plain `adb shell`** — Android's shell is bionic-
  linked and can't resolve/run the ubuntu rootfs's glibc `journalctl` via bare `PATH` lookup. Any
  `journalctl --vacuum-time=…`/`--rotate` run this way **silently no-ops** if you've piped it through
  `2>/dev/null` (as an earlier "clear the logs" pass in this session did) — it looks like it succeeded
  but nothing happened. **To actually clear the journal, `rm -f` the `.journal`/`.journal~` files
  directly** (they're just files under
  `/data/.stowaway/<distro>/var/log/journal/<machine-id>/`) — no need for `journalctl` at all for
  clearing.
- **To *read* the journal, pulling the file and running the *host's* `journalctl` can fail even on a
  structurally-valid file** — this session's Arch host runs systemd 261 vs. the ubuntu rootfs's systemd
  255.4; opening the pulled `system.journal` gave `Failed to open files: No data available` even after
  a clean re-pull. **Use the device's own matching `journalctl` instead**, via
  `adb shell chroot /data/.stowaway/<distro> /usr/bin/journalctl -D /var/log/journal …` — this worked
  and is now the established method. (Chroot + exec of a glibc aarch64 binary works fine from Android's
  root shell regardless of libc mismatch; the kernel doesn't care, only the shell's own bare `PATH`
  lookup does.)
- Even with the matching version, a **genuinely truncated/corrupted journal** (`system.journal is
  truncated, ignoring file`) will still refuse to open — that's real evidence of an unclean shutdown,
  not a version problem. See §5.

---

## 5. Latest cold-boot ubuntu attempt — reached real Android HAL bring-up, then crashed unexplained

After the §3 fix was deployed, user did a real switch_root cold boot into ubuntu. Findings:

- `kaos-droid-hal-init.log` (plain text, journal-independent) shows the **entire** cold-boot sequence
  completing cleanly through every step (mounts, APEX fallback, selinuxfs, all three rc patches),
  reaching the final `exec droid-hal-init` — so §1's fixes held up fully on this run.
- **`system.journal` (PID 1 / systemd's own boot log) ended up truncated/corrupted** — even the
  device's own matching `journalctl 255.4` (via chroot) reported
  `Journal file … is truncated, ignoring file` for it. This means the session did **not** shut down
  cleanly.
- `persist.sys.boot.reason.history` shows a bare **`reboot,16884835`** entry (no `shell` suffix)
  sandwiched between two `reboot,shell,…` entries (which were the ones I triggered via `adb reboot`) —
  consistent with something *inside* the switched-root session forcing a reboot on its own, rather than
  a normal `adb`/UI-triggered one. This is likely what truncated the journal, but the journal itself
  doesn't survive to say what.
- `user-1000.journal` (phosh's user session, uid 1000) **did** open cleanly and shows real activity:
  `dbus.service` crash-loops on `Failed to open "/etc/selinux/targeted/contexts/dbus_contexts": No such
  file or directory` → hits systemd's start-rate-limit → `pipewire`/`wireplumber` then fail to reach the
  (now-dead) session bus. This SELinux-contexts file gap is real and independent of the crash — a
  concrete, actionable lead if picked up next.
- **Not established:** what specifically triggered the uncommanded reboot after `droid-hal-init` took
  over — it's somewhere past that handoff point, deeper into Android HAL bring-up or systemd's own
  continuation, and the corrupted system journal doesn't preserve it.

---

## 6. Files changed this session

All under `/home/jimmy/hadk` (no git at repo root; verify via the checksums below rather than `git
diff`):

- `kaos/scripts/kaos-service` — `ns-entry.sh` heredoc: `touch /etc/kaos-overlay-active` before
  `exec $INIT`. md5 `b62046ce14b304f1a72effe7bcfb01be`. **Also pushed to `/system/bin/kaos-service`
  on-device** (matching md5 confirmed) — this is a binary-on-`/system` deploy, not just a rootfs file;
  remember `adb remount` first.
- `hybris/droid-configs/sparse/kaos.init` (oracle) — `rm -f /etc/kaos-overlay-active` at `=== START
  ===`. md5 `a3bc17511b8e38f12306ab200ac69e11`.
- `kaos/apps/phosh-app/res/raw/droid_dm_setup.sh` (mirror) — same change, byte-identical. md5
  `a3bc17511b8e38f12306ab200ac69e11` (matches oracle — `kaos-init-drift-check.sh` confirms in sync).
- `hybris/droid-configs/sparse/usr/lib/systemd/system/kaos-droid-hal-init.service` — added
  `ConditionPathExists=!/etc/kaos-overlay-active` + rationale comment. md5
  `06a903a6b7583f3f4aef74caba4e91af`.
- Same one-line `ConditionPathExists=!/etc/kaos-overlay-active` addition (+ short comment pointing back
  at the `.service` file) to all 7 ported mount units:
  - `system_root.mount` md5 `f55bf415869bdcd650d9f7734fccc693`
  - `system.mount` md5 `d1e7a3d2b7e80089a41de7b76a777e6e`
  - `vendor.mount` md5 `c4d0e2a5fc7d012eec066d7a500eb115`
  - `vendor-firmware_mnt.mount` md5 `2ad706d7651a1774271d6c9c12ed944e`
  - `system_ext.mount` md5 `f458229e08682e503b454ee722867244`
  - `product.mount` md5 `94edef9d41dce03f84881e08aacc71d2`
  - `odm.mount` md5 `162211558437f2efd6b7156299ec7880`

`hybris/kaos-configs/build-rootfs.sh` already installs all 8 of these files into every built rootfs
(wired in 2026-09-21) — **no build-rootfs.sh changes needed this session**, the edits above are to files
it already copies verbatim.

All 8 files above were live-pushed to the on-device ubuntu rootfs (`/data/.stowaway/ubuntu/…`) and
md5-verified to match source at push time. **None of this is in a real rebuilt rootfs/ROM yet** — user
owns builds; next `mka bacon` + rootfs rebuild should pick these up automatically since they're already
wired into `build-rootfs.sh`'s install loop.

---

## 7. On-device state (as of session end)

- Device is back in plain Android (`ro.boot.bootreason=reboot,shell`, `PID 1 = android_init`) after the
  §5 cold-boot crash-reboot.
- `kaos-bridge` (port 30000) is running — was found stopped once mid-session (leftover from an
  interrupted `&&` command chain around the `/system/bin/kaos-service` push) and had to be restarted
  with `adb shell start kaos-bridge`. If a bridge `nc` query ever gets `Connection refused`, check this
  first.
- Ubuntu journal was cleared (both `rm -rf` on the journal dir and confirmed via a fresh empty listing,
  not just `journalctl --vacuum` which — per §4 — silently does nothing over plain `adb shell`) right
  before the §5 cold-boot attempt, so the truncated `system.journal` described in §5 is genuinely from
  that one attempt, not stale leftovers.
- `/etc/kaos-overlay-active` marker is currently **absent** on-device (correctly removed by `kaos.init`
  during the §5 cold boot) — this is expected/correct state, not a bug.
- Sailfish cold-boot was last confirmed working as of 2026-09-21 — not re-tested this session (all
  cold-boot testing this session was ubuntu-only).

---

## 8. Suggested next steps (priority order)

1. **Re-test cold-boot WLAN with more patience** past `icnss: QMI Server Connected` (§1) — the single
   biggest open question is whether `wlan0` appearing is just a timing gap or a further real blocker.
2. **Chase the §5 uncommanded-reboot / truncated-journal mystery** — needs either a way to capture the
   crash before the auto-reboot wipes context (e.g. redirect systemd's own journal forwarding to a
   plain-text file as a fallback, or `SYSTEMD_LOG_TARGET=kmsg` for early boot so `dmesg`/pstore has a
   chance of catching it), or a serial/physical console if available.
3. **Fix the `/etc/selinux/targeted/contexts/dbus_contexts` missing-file gap** (§5) — concrete,
   independent of the crash, currently crash-looping `dbus.service` in the user session every cold boot.
4. **Decide the cold-boot graphics direction** (§2, carried over from 2026-09-21 §5) — still explicitly
   undecided; don't pick without the user.
5. Fix the separate `/dev/input/eventN: Permission denied` cold-boot input issue (§2) — simpler, not
   yet investigated.
6. Once WLAN + graphics are both resolved for ubuntu cold boot, fold the `/etc/kaos-overlay-active`
   marker mechanism understanding into a real rootfs rebuild + ROM build so it's no longer just
   live-patched.

---

## 9. Access / build / deploy notes (carried over + amended)

- **User owns builds** — never run `mka`/`soong_ui.bash`/kernel `make` unless explicitly asked, even to
  verify a patch.
- **`adb root` doesn't persist across reboots** — re-run it after every `adb reboot`/device reconnect
  before doing anything that needs root.
- **`/system` is `ro` at boot** — `adb remount` before pushing to `/system/bin/*`. Watch for `&&`-chained
  commands that include a `stop <service>` before a step that can fail (RO push failure, etc.) — the
  chain aborts and the service is left stopped. Always verify with a follow-up status/port check.
- **Journal handling — see §4 in full.** Short version: delete `.journal` files directly to clear (not
  `journalctl --vacuum` over plain `adb shell`, which silently no-ops); read via
  `adb shell chroot /data/.stowaway/<distro> /usr/bin/journalctl …` (matching version) rather than
  pulling to the host, unless you've confirmed the host's systemd version matches.
- **Boot-environment sanity check** (useful whenever it's ambiguous whether a shell is talking to
  Android or a switched-root distro): `cat /proc/1/comm` + `tr '\0' ' ' < /proc/1/cmdline` +
  `getprop ro.boot.bootreason`. `android_init` / `/system/bin/init second_stage` / any `bootreason`
  value at all means you're in Android (a genuine switch_root replaces PID 1 entirely, so
  `getprop`/Android bootreason properties wouldn't resolve the same way).
- **Cold-boot logs to read after a failed boot** (unchanged from 2026-09-21, still accurate):
  `/data/.stowaway/ubuntu/var/log/kaos-droid-hal-init.log` (this session's adapter, plain text, survives
  journal corruption), `/data/.stowaway/ubuntu/var/log/boot.log`, and the journal per §4's method.
- `hybris/mw/mesa-freedreno/rpm/` is SailfishOS-only — standing instruction, never touch.
