<!-- Parent: AGENTS.md -->

# Distro Boot Subsystem

> Root bridge, namespace lifecycle, distro registry, and Android apps. This subsystem predates the broader Kaos scope and still carries the `kaos` path name; it now serves Kali, SailfishOS, Ubuntu Touch, Droidian, and future distros.

## Constraints

- **Root Access via Port 30000:** Root access is strictly handled via the localhost TCP bridge — do not call the `su` binary directly.
- **Kernel 4.9 Compatibility:** Always verify the kernel version. On kernel 4.9, systemd fails due to cgroup v2 delegation constraints; use the `kaos-init` (or `kali-compat-init`) fallbacks.
- **Safe Seeding:** [distros.conf](file:///home/jimmy/hadk/kaos/scripts/distros.conf) copies must check `[ -f ... ]` to avoid wiping user registry changes.
- **Data Encryption Bypass:** Any directory under `/data/` that is needed before FBE keys are unlocked (e.g. `/data/.stowaway`) must be declared with `encryption=None`.
- **Bridge Context:** The bridge service must maintain the `seclabel u:r:init:s0` directive.
- **Dynamic Partition Detection:** Android partition block paths are resolved at boot by the `/kaos.init` hook (`hybris/droid-configs/sparse/kaos.init`, run by the trampoline before init handoff), which populates `/run/droid/*` for the native `.mount` units. The old `droid-mount-setup.service` was removed 2026-08-17 — do not reintroduce `Requires=`/`After=droid-mount-setup.service` into the `.mount` units or `droid-hal-prepare.service`.

## Working Here

Scripts in [scripts/](file:///home/jimmy/hadk/kaos/scripts/) can be hot-deployed without a full build:
```bash
adb push kaos/scripts/{script} /system/bin/
adb shell "chmod 755 /system/bin/{script}"
```

Verify bridge after any bridge/init.rc change:
```bash
adb shell "netstat -an | grep 30000"
adb shell "echo id | nc 127.0.0.1 30000"
```

Verify namespace after service changes:
```bash
adb shell "cat /data/.stowaway/kali/var/log/ns-entry.log | tail -20"
```

## Session Handoffs

Dated handoff notes from prior sessions live in this directory. **Read the latest before working
here** — several changes only make sense together:

- [SESSION_HANDOFF_2026-09-21.md](file:///home/jimmy/hadk/kaos/SESSION_HANDOFF_2026-09-21.md) —
  unify-distro-support-engine tasks landed, `/kaos.init` cold-boot adapter, single-namespace guard,
  ubuntu cold-boot bring-up + the KGSL↔DRM/Halium graphics decision.
- [SESSION_HANDOFF_2026-09-19.md](file:///home/jimmy/hadk/kaos/SESSION_HANDOFF_2026-09-19.md) —
  clone3/TLS kernel fix, namespace teardown watchdog + `reap`.
- [SESSION_HANDOFF_2026-09-18.md](file:///home/jimmy/hadk/kaos/SESSION_HANDOFF_2026-09-18.md)
- [SESSION_HANDOFF_2026-09-16.md](file:///home/jimmy/hadk/kaos/SESSION_HANDOFF_2026-09-16.md)

## Key Files

| File | Purpose |
|------|---------|
| [kaos-bridge](file:///home/jimmy/hadk/kaos/scripts/kaos-bridge) | Port 30000 loopback root bridge server. |
| [kaos-service](file:///home/jimmy/hadk/kaos/scripts/kaos-service) | Namespace lifecycle daemon manager. |
| [kaos-starter](file:///home/jimmy/hadk/kaos/scripts/kaos-starter) | Command-line client for namespaces. |
| [kaos-chroot-install](file:///home/jimmy/hadk/kaos/scripts/kaos-chroot-install) | Helper for extracting distribution tarballs. |
| [distros.conf](file:///home/jimmy/hadk/kaos/scripts/distros.conf) | INI-based registry of installed distributions. |
| [distros-lib.sh](file:///home/jimmy/hadk/kaos/scripts/distros-lib.sh) | Shell script library to parse `distros.conf`; `resolve_distro` + `distro_manifest` (conf keys > profile file > generic). |
| [distro-profiles/](file:///home/jimmy/hadk/kaos/scripts/distro-profiles/) | Per-distro `PROFILE_*` modules (fallback/example set; `distros.conf` behavior keys are authoritative). |
| [device.conf](file:///home/jimmy/hadk/kaos/scripts/device.conf) | Device hardware declaration (DRM nodes/majors, display size, Mesa/KGSL prefix); sourced by `kaos-service` and `/kaos.init`. |
| [kaos-init-drift-check.sh](file:///home/jimmy/hadk/kaos/scripts/kaos-init-drift-check.sh) | Byte-identity guard between the `/kaos.init` oracle and its `res/raw` mirror. |
| /kaos.init | Cold-boot adapter (`hybris/droid-configs/sparse/kaos.init`): dm-node refresh + feature-detected `adapt_boot` (SELinux, DRM nodes, phosh wiring; skips sailfish). Runs from the trampoline before init handoff. |
| [sailfish-fixup.sh](file:///home/jimmy/hadk/kaos/scripts/sailfish-fixup.sh) | DEPRECATED (July 2026) — do not run. Rootfs is built for perseus; the script's Mesa/eglfs compositor rewrite clobbers the hwcomposer QPA display config. No longer invoked by kaos-service. |
| [kali-preinit](file:///home/jimmy/hadk/kaos/scripts/kali-preinit) | Early boot wrapper (PID 1) within the Kali namespace. |
| [kali-compat-init](file:///home/jimmy/hadk/kaos/scripts/kali-compat-init) | Fallback PID 1 loop for kernel 4.9. |
| [kali-hybridos-prepare](file:///home/jimmy/hadk/kaos/scripts/kali-hybridos-prepare) | Systemd state preparation script for systemd boot on Kali. |
| [kali-hybridos-verify](file:///home/jimmy/hadk/kaos/scripts/kali-hybridos-verify) | Handoff verification check for mounts within the Kali distro. |
| [kaos-phosh](file:///home/jimmy/hadk/kaos/scripts/kaos-phosh) / [phosh-start](file:///home/jimmy/hadk/kaos/scripts/phosh-start) | Desktop session starting wrappers. |
| [init.kaos.rc](file:///home/jimmy/hadk/kaos/init.kaos.rc) | Android init service rules. |
| [init.extraenv.arm64-v8a.rc](file:///home/jimmy/hadk/kaos/init.extraenv.arm64-v8a.rc) | Environmental helpers for target CPU architecture. |
| [apexd-hybris.rc](file:///home/jimmy/hadk/kaos/apexd-hybris.rc) / [logd-hybris.rc](file:///home/jimmy/hadk/kaos/logd-hybris.rc) / [servicemanager-hybris.rc](file:///home/jimmy/hadk/kaos/servicemanager-hybris.rc) | Hybris-specific AOSP services overrides. |
| [hybris_display.te](file:///home/jimmy/hadk/kaos/sepolicy/vendor/hybris_display.te) | SELinux policy overrides for display bridge. |

## Dependencies
- **Depends on:** AOSP init config, custom SELinux vendor policy rules.
- **Depended on by:** Distro installer/manager apps (including legacy NetHunter Manager), NeoTerm Emulator interface, early boot ROM selector UI.
