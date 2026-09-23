# Session Handoff — 2026-09-21 (unify distro engine + `/kaos.init` cold-boot adapter + ubuntu cold-boot bring-up)

Subtitle: Completed the `unify-distro-support-engine` plan (Tasks 1–14, F2–F4), promoted `/kaos.init`
to the feature-detected cold-boot adapter, added a single-namespace guard, and drove the ubuntu
cold-boot bring-up from "systemd dies instantly" to "phoc starts but EGL can't load a driver" —
root-causing the last blocker as the missing KGSL↔DRM translation, which drives a decision to make
the graphics stack a **Kaos standard, not a Droidian special case**.

**Read this before touching `kaos/scripts/kaos-service`, `kaos/scripts/distros-lib.sh`,
`hybris/droid-configs/sparse/kaos.init` (and its `kaos/apps/phosh-app/res/raw/droid_dm_setup.sh`
mirror), `system/extras/multirom/trampoline/trampoline.c`, or
`docs/exec-plans/active/unify-distro-support-engine.md`.** Several changes only make sense together.

No git at the repo root; sub-repos (`hybris/kaos-configs`, `kaos/`, `kernel/*`) are git. State below
is direct file state + on-device verification as of session end.

---

## 1. `unify-distro-support-engine` plan — Tasks 1–14 + F2–F4 landed

Plan: `docs/exec-plans/active/unify-distro-support-engine.md`. Status now: **1–5, 7–13 ✅; 6, 14 🟡
(hardware cold-boot half only); F2–F4 ✅; F1 (warm reboot) deferred by user.**

What landed (all `sh -n`/`bash -n` green, most verified live or via harness):

- **Task 3** — Kali's init-ordering moved to profile data: `kaos-service` ns-entry candidate order is
  now `$PROFILE_INIT_PRIORITY` (the `DISTRO_ID = kali` gate is gone); `kaos-chroot-install`'s wrapper
  install is gated on `$PROFILE_INSTALL_WRAPPERS` (the `DISTRO_NAME = kali` gate is gone). Critically,
  `load_distro_profile`/`distro_manifest` is now **actually called** in both consumers and its
  `PROFILE_*` vars are **exported**, so they reach the `ns-entry.sh` child (previously never loaded →
  every `PROFILE_UI` read was dead).
- **Task 4** — `distro-profiles/{droidian,ubuntu-touch}.sh` declare `PROFILE_HALIUM=1` +
  `PROFILE_MOUNTS_HOST=""`; `load_distro_profile` derives `PROFILE_HALIUM=1` from legacy
  `type=halium`.
- **Task 5** — aliases (`kali-pro`/`kali_native`/`kali-native`) + no-config fallback → kali profile
  (harness-verified).
- **Task 7** — `distros.conf` header documents the six behavior keys (`host_mounts/ui/mask_extra/
  init_priority/kaos_init/halium`) with safe defaults.
- **Task 8** — `resolve_distro()` reads the six keys into `DISTRO_*`; new **`distro_manifest`** applies
  precedence **distros.conf keys > profile file > generic**; `load_distro_profile` kept as a back-compat
  alias. Harness-verified all three tiers.
- **Task 9** — every distro-behavior read in `kaos-service`/ns-entry is `PROFILE_*`; zero inline
  distro-identity branches remain.
- **Task 10** — trampoline parser reads `halium`/`ui`/`kaos_init` into `struct distro_behavior` with
  `type=halium`→`halium=1` fallback; all three `HYBRIDOS_BOOT_SIGNAL` readers use `parse_boot_signal`
  (legacy `path|type` still parses); the three halium sites use `behavior->halium`; `/kaos.init` gated
  on `kaos_init` (−1 auto / 0 off / 1 on). **Built + deployed to both `/system/bin/init` and
  `/system/bin/trampoline`** (md5 `7b3ebff0…`). Runtime-validated by the user cold-booting **sailfish**.
- **Task 11** — `kaos-chroot-install` post-extract wiring now goes through a generic,
  failure-propagating `install_post_extract_hooks` (manifest-driven).
- **Task 12** — `hybris/kaos-configs/build-rootfs.sh` applies a manifest overlay (reads
  `hostname`/`codename`/`mirror` from `kaos/scripts/distros.conf`); vendored `droidian-configs`/
  `ubports-configs` build-rootfs.sh carry convergence notes.
- **Task 13** — `/kaos.init` oracle ≡ mirror byte-identical; `kaos/scripts/kaos-init-drift-check.sh`
  guards it.
- **F2** — new **`kaos/scripts/device.conf`** device declaration (`DEV_DRM_CARD/RENDER` + majors/minors,
  `DEV_DISPLAY_SIZE`, `DEV_MESA_KGSL`); sourced+exported by `kaos-service` with perseus defaults; DRM
  mknod nodes, `DISPLAY_SIZE` fallback, kaos-init env exports, and (via post-write `sed`) `phoc.ini`
  mode + `phosh-session`/phosh.service paths all read it.
- **F3** — `kaos/AGENTS.md` "Dynamic Partition Detection" rewritten to `/kaos.init` + native `.mount`
  units (with a do-not-reintroduce `droid-mount-setup.service` warning).
- **F4** — kaos-init core-service startup feature-detects the service manager (`/etc/init.d` → OpenRC
  `rc-service` → `service` → `systemctl`, ssh/sshd alias) instead of assuming Debian.

---

## 2. Ad-hoc fixes (outside the plan)

- **Single-namespace guard** (`kaos/scripts/kaos-service`). Root-caused while verifying the watchdog:
  helper-daemon PID files (`/data/local/tmp/kaos-input.pid`, `kaos-display-bridge.pid`) are **global**
  while the namespace PID file is per-distro, so two overlapping namespaces clobbered the helpers and
  orphaned daemons. Added a `running_namespace()` scanner (excludes the helper PID files);
  `start_namespace` now **refuses if ANY distro's namespace is live**; `show_status` reports a foreign
  running namespace. Verified live (2nd start refused; stop leaves no orphans).
- **Watchdog re-verified live**: killing namespace PID 1 fires `teardown_all` (mounts unmounted, PID
  file removed, status → Inactive). The watchdog itself was already correct.
- **`kaos-init-drift-check.sh` bug**: nonfatal-drift branch had `exit aglia0` (invalid) → `exit 0`.
- **journald `Storage=persistent` bug** (`build-rootfs.sh`): the old
  `sed 's/^#Storage=persistent/…/'` was a **no-op** against the stock `#Storage=auto`. Now
  `sed -E 's/^#?Storage=.*/Storage=persistent/'` + `install -d /var/log/journal`. (Journals were
  persisting only by luck — the `auto` directory probe found `/var/log/journal` already present.)
- **arm64 binfmt preflight** (`build-rootfs.sh`): the host (Arch x86_64) had no `qemu-aarch64` binfmt
  handler, so cross-arch debootstrap's 2nd stage died with `chroot: … Exec format error`. Added a
  host-side `ensure_arm64_binfmt()` (checks `/proc/sys/fs/binfmt_misc/qemu-aarch64`; falls back to
  `update-binfmts`, then `docker run --privileged --rm tonistiigi/binfmt --install arm64`; errors
  loudly with per-distro fix commands). Also un-swallowed the in-container `update-binfmts` failure.
  **Note for the host: `qemu-user-static` alone doesn't register binfmt on Arch — you need
  `qemu-user-static-binfmt` + `systemd-binfmt`, or re-run the tonistiigi command after each reboot.**

---

## 3. `/kaos.init` promoted to the cold-boot adapter

User direction: the cold-boot wiring belongs in `/kaos.init`, feature-detected — not in the build.
`hybris/droid-configs/sparse/kaos.init` (oracle; `kaos/apps/phosh-app/res/raw/droid_dm_setup.sh` is the
byte-identical mirror) now runs `refresh_nodes` **+ `adapt_boot`**, and `adapt_boot`:

- reads device constants from `/usr/local/share/kaos/device.conf` (seeded by the build) with perseus
  defaults;
- **SELinux**: if `/etc/selinux` exists but `config` is missing, writes `SELINUX=disabled` (this was a
  real cold-boot killer — see §4);
- **DRM nodes**: creates-if-missing and `chmod 0666`s `card0`/`renderD128`;
- **render group**: adds the uid-1000 user to `render` if the group exists (udev sets `renderD*` to
  `GROUP=render MODE=0660`);
- **phosh distros**: enables `phosh.service` on `graphical.target`, writes the cold-boot
  `phosh.service.d/kaos.conf` (env now matches the overlay drop-in exactly, only
  `WLR_BACKENDS=drm,libinput` differs), writes `/etc/phosh/phoc.ini` (`[output:DSI-1]`), and **clears**
  any overlay frame-relay wiring (`kaos-ui-stream*`) left by a previous warm start;
- **sailfish**: detected (`/usr/bin/lipstick` or `os-release ID=sailfishos`) → **skipped** entirely;
- logs to `/dev/kmsg` **and** a persistent `/var/log/kaos-coldboot.log` inside the rootfs.

`build-rootfs.sh` now mounts the sparse oracle and seeds `/kaos.init` + `device.conf` into every built
rootfs. Both copies kept byte-identical; drift check green.

**Warm/overlay is unaffected**: overlay runs `kaos-service` (not `/kaos.init`); `adapt_boot` is
cold-only. And overlay already reaches a full phosh session — its systemd runs in container mode
(`ConditionVirtualization=!container`), so it never tries to load the SELinux policy.

---

## 4. Ubuntu cold-boot bring-up — progression + the current blocker

Every step below was read out of the persistent distro journal
(`/data/.stowaway/ubuntu/var/log/journal/…`, `Storage=persistent`) and/or `/var/log/boot.log`
(the trampoline redirects stdout/stderr there before exec'ing init) and `/var/log/kaos-coldboot.log`.

1. **systemd died instantly** with only `SELinux: Could not open policy file <= …/policy.33` in
   `boot.log`. Root cause: the distro has SELinux userspace but **no `/etc/selinux/config`**, so
   libselinux/systemd tried to load a missing policy. Sailfish cold-boots because it ships
   `SELINUX=disabled`. → **fixed** in `adapt_boot`. systemd then ran as **real PID 1**
   (`ConditionVirtualization=!container succeeded`), captured the kernel log, reached
   `graphical.target`.
2. **phoc couldn't get a seat** (`[libseat] Could not activate session: Interactive authentication
   required` / `Could not open tty0 … Permission denied`). No `seatd` installed, logind has no VT. →
   **fixed** by using the same `kaos-libseat-fake.so` LD_PRELOAD shim as overlay (its
   `libseat_open_device` hands wlroots the real `/dev/dri/card*`).
3. **phoc couldn't open `/dev/dri/renderD128`** (`Permission denied`): cold-path `/dev` is a fresh
   devtmpfs (`root:root 0600`), phoc runs as uid 1000. → **fixed** by the DRM-node `mknod`/`chmod`.
4. **Still `Permission denied` on `renderD128`**: `systemd-udevd` re-applies `50-udev-default.rules`
   (`renderD*` → `GROUP="render" MODE="0660"`); uid 1000 was in `video` (so `card0` was fine) but not
   `render`. → **fixed** by adding the uid-1000 user to `render`.
5. **phoc then opened DRM but EGL failed**: `[EGL] eglInitialize: EGL_NOT_INITIALIZED, "DRI2: failed
   to load driver"` → `Could not initialize renderer` → `phosh.service` exit 1, restart loop. Aligned
   the cold drop-in env with the overlay drop-in (`VK_ICD_FILENAMES`, `MESA_DRIVERS_PATH`,
   `LIBGL_DRIVERS_PATH`, `GBM_BACKENDS_PATH=$DEV_MESA_KGSL/lib/gbm`, `XDG_SEAT=seat0`,
   `WLR_DRM_NO_MODIFIERS=1`) — **still fails**.

**Root cause of the remaining blocker (conclusion):** the graphics stack can't bridge Android's KGSL
GPU (`/dev/kgsl-3d0`, what the KGSL mesa build drives) with the Linux DRM/GBM/KMS device
(`/dev/dri/card0`, `msm_drm`, display-only). The kaos `phosh-session` preloads
`kgsl_drm_shim.so` + `kaos-drm-redirect.so` **only in DRM mode** for exactly this — and those files
**do not exist anywhere**: no source, no binary, no git history (only the `LD_PRELOAD` strings in
`kaos/scripts/kaos-service` L719/L795). They were never shipped; the DRM path has been referencing
phantoms. Overlay never needs them (headless via KGSL into a buffer, no KMS).

---

## 5. Direction: make the graphics stack a **Kaos standard**, not a Droidian special case

User directive: *"change it from being a droidian stuck to being a standard for kaos."* Research done
this session:

- **Halium graphics already works in this tree.** `external/libhybris/AGENTS.md`: the `hwc2`
  (hwcomposer), `input`, `surface_flinger`, `ui` compat layers are **functional on Android 15**;
  they're "depended on by Linux distros needing GPU/display."
- **Droidian-on-Halium for perseus exists**: `hybris/droidian-configs`
  (`HALIUM_VARIANT=hybris-15.0`, `ANDROID_BASE=15`, `KERNEL_VERSION=4.9`), and Droidian runs phosh via
  a **`phosh-config-hwcomposer`** package — i.e. wlroots' **hwcomposer backend** → libhybris →
  Android hwcomposer + Adreno EGL. That's the proven "phosh on Halium" path and it sidesteps the
  KGSL↔DRM problem entirely (no mesa/GBM/KMS).
- **What's Droidian-coupled today** and should be lifted out: `kaos/apps/phosh-app/…/SetupManager.java`
  (hundreds of lines of libhybris/EGL/composer hacks), `device/xiaomi/perseus/droidian_perseus.mk`
  (a lunch target just to gate the composer HAL), and `kaos/hal/hwcomposer-null` (a null composer HAL,
  now "unused").

**Proposed Kaos standard (not yet implemented — design only):**
1. Android side, always-on: ship the composer HAL (real, or the null one) + gralloc + GPU blobs + the
   `hwc2`/`sf`/`ui` compat layers for every build (no Droidian gate).
2. A Kaos graphics runtime layer (e.g. `kaos/gfx/`): distro-neutral libhybris + `wlroots-hwcomposer` +
   hwcomposer config, with the libhybris linker/EGL setup currently in `SetupManager.java` moved into
   a script any distro / `/kaos.init` runs.
3. Manifest opt-in: a behavior key `gfx=hwcomposer|mesa|none` (parallel to `halium=`/`ui=`) in
   `distros.conf` + profiles, exposed by `distro_manifest`, read by the trampoline and `/kaos.init`.
4. `/kaos.init` feature-detects `gfx=hwcomposer` → sets up the Halium graphics env
   (`WLR_BACKENDS=hwcomposer`) instead of the DRM/mesa path.
5. Retire `droidian_perseus.mk` gating and the Droidian hacks in `SetupManager.java`.

Open decisions raised (unanswered): shared Kaos libhybris+wlroots-hwcomposer bundle vs per-distro
build; `gfx=` key vs folding into `halium=1`; whether to do the Android-side ungating + plumbing first
and defer wlroots-hwcomposer packaging.

**Note the tension:** Phase 1 of the unify plan deliberately rebuilt droidian/UT as plain **mainline**
(non-Halium) for the namespace/overlay approach. Adopting the Halium graphics path re-introduces the
Halium/LXC path for graphics — it's a direction decision.

---

## 6. On-device state (as of session end)

- **ubuntu rootfs**: rebuilt + deployed from `droid-local-repo/perseus/kaos/kaos-ubuntu-rootfs.tar.xz`
  (693 MB, 2026-09-21 15:34) via `kaos-chroot-install --distro ubuntu -c "restore …"`. Contains
  `/kaos.init` (byte-identical to oracle) + `/usr/local/share/kaos/device.conf`. A later `/kaos.init`
  update was hot-pushed directly (md5 below).
- **Deployed files (md5-identical to source where noted):**
  - `/system/bin/kaos-service` `e1ad5543…` (= source)
  - `/system/bin/distros-lib.sh` `28a9bcf6…` (= source)
  - `/system/bin/kaos-chroot-install` `c895e043…` (= source)
  - `/system/bin/init` = `/system/bin/trampoline` `7b3ebff0…` (= Task-10 build)
  - `/data/.stowaway/ubuntu/kaos.init` `f27f45f8…` (= oracle; source and deployed match at
    closeout — this includes the EGL/env alignment).
- **Backups**: `/data/local/tmp/kaos-backup-20260920/` holds the pre-session `init`, `trampoline`,
  `kaos-service`, `distros-lib.sh`, `kaos-chroot-install`.
- **Boot selector**: `multirom.ini` `auto_boot_rom=ubuntu`, `auto_boot_seconds=5`; `/mnt/vendor/persist/
  hybridos_boot_distro` was found stale (`droidian`) and set to `ubuntu`. **Caveat: the trampoline's
  `BOOT_DISTRO_FILE` direct-boot check lives only in `mount_and_run()`, not in `run_init_second_stage()`
  (the actual cold path)** — so the direct-boot file does nothing for cold boot; the selector is what
  matters. Recent `adb reboot`s repeatedly timed out at the selector ("no distro selected - booting
  Android", `multirom` exit 768) unless a distro was picked.
- **Logs cleared** (runtime logs + `logcat` + persistent journals) before the last reboot; the ubuntu
  journal at closeout was from the most recent cold-boot attempt.
- **Sailfish cold-boot is confirmed working** (user) on the current trampoline — the one distro whose
  cold boot is verified.

---

## 7. Suggested next steps (priority order)

1. **Decide the graphics direction (§5)**: Kaos-standard Halium/hwcomposer vs the KGSL↔DRM shim vs
   wlroots-hybris-EGL. This unblocks ubuntu cold-boot graphics.
2. If Halium/hwcomposer: start with the **Android-side ungating + `gfx=` manifest plumbing +
   `/kaos.init` feature-detection**, then tackle wlroots-hwcomposer packaging.
3. If shim: write `kgsl_drm_shim.so` (bridge wlroots' GBM/EGL render path to KGSL while scanout uses
   `card0`) — it has never existed here.
4. **Nothing to re-deploy right now** — `/kaos.init` source == deployed (`f27f45f8…`) at closeout.
   The next cold-boot test can run as-is (it will reproduce the EGL blocker until §5 is decided).
5. Close Tasks 6/14's hardware half when kali/droidian/UT are installable (sailfish already verified).
6. Investigate the **~17–19 min stall** observed in the cold-boot journal before `phosh.service`
   starts (systemd held on `sysinit.target`/`basic.target`); and `gdm-x-session` appearing alongside
   phosh (possible gdm conflict).
7. F1 (warm `reboot hybridos,<distro>` kaos-way) remains deferred by the user.

---

## 8. Access / build / deploy notes

- **Ask before device-affecting actions.** User explicitly required permission before rebooting or
  writing to the device (reboot, deploy, `/data`/`/system` writes). `adb reboot` without asking is a
  mistake that already happened once this session.
- **Root**: `adb root` works (doesn't persist across reboots). `/system` is `ro` at boot;
  `adb remount` (or `mount -o remount,rw /`) to hot-deploy to `/system/bin`.
- **Trampoline deploy (per AGENTS.md)**: push to **both** `/system/bin/init` and
  `/system/bin/trampoline`. Build: `source build/envsetup.sh && lunch lineage_perseus-bp1a-userdebug &&
  build/soong/soong_ui.bash --make-mode trampoline_system`.
- **Cold-boot logs to read after a failed boot**: `/data/.stowaway/ubuntu/var/log/kaos-coldboot.log`
  (adapter trace), `/data/.stowaway/ubuntu/var/log/boot.log` (trampoline stdout/stderr),
  `/data/.stowaway/ubuntu/var/log/journal/…` (persistent distro journal — pull it and use host
  `journalctl -D`). `dmesg` on Android only holds the cold-attempt kmsg if the trampoline fell back to
  Android **without** rebooting.
- **Clearing logs** (user asks often before a test): truncate `/data/adb/kaos-service.log`, per-distro
  `var/log/{ns-entry,namespace-boot,kaos-init,kaos-coldboot,boot,kaos-display-bridge,kaos-touch-input,
  touch-input,seatd}.log`, `/data/.stowaway/{distro_load_debug,fb_debug,multirom_ui_debug}.log`,
  `logcat -c`, and **remove** (not truncate) the `var/log/journal/*/*.journal` files.
- **Build ownership**: the user builds rootfs/kernel/ROM themselves by default; only build when asked.
  The host is Arch x86_64 — remember the arm64 binfmt caveat (§2).
- `hybris/mw/mesa-freedreno/rpm/` is SailfishOS-only — never touch, standing instruction.
- `docs/exec-plans/active/unify-distro-support-engine.md` has the full task table + dated Progress
  Notes for this session.
