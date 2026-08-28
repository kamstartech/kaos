# Platform Probe Research Findings — Kaos/Perseus Halium Boot Mode

Research date: 2026-08-01. Research-only task; no files were modified.

Scope: Follow-on to the TLSDESC fix and the libdl.so fix. All libhybris loader
blockers are resolved (libcutils/liblog/libbase/libc++/libc/libm/libdl and
ld-android.so all link cleanly per HYBRIS_LD_DEBUG=1), yet the
`ubports:android2` graphics platform module
(mir-android2-platform v1.8.0, `graphics-android2.so.15`, Mir 1.8.3) reports
`Support priority: 0` during `probe_graphics_platform`, so Mir throws
`Failed to find platform for current system` at `platform_probe.cpp(119)` and
never even attempts to dlopen any HAL/gralloc/hwcomposer/EGL library.

## Bottom line

1. **The probe in this build does exactly ONE check, and it is a system-property
   read, not a HAL probe.** Since commit `5831ded` ("platform: makes Mir choose
   us over the old code on Android 8+", 2021-12-28) — the same commit that
   renamed the module to `android2` and produced package v1.8.0 — the probe is:

   ```cpp
   // src/platforms/android/server/platform.cpp:358-379 (identical at 5831ded and main)
   static int get_android_api_level()
   {
       char propval[PROP_VALUE_MAX];
       if (::property_get("ro.build.version.sdk", propval, "") < 0)
           return -1;
       return atoi(propval);
   }

   mg::PlatformPriority probe_graphics_platform(...)
   {
       mir::assert_entry_point_signature<mg::PlatformProbe>(&probe_graphics_platform);
       if (get_android_api_level() >= 26) { // Android 8 = API level 26.
           // Trumps the old mir-android-platform's confidence.
           return static_cast<mg::PlatformPriority>(mg::PlatformPriority::best + 16); // = 272
       } else {
           return mg::PlatformPriority::unsupported; // = 0
       }
   }
   ```

   `PlatformPriority` enum: `unsupported=0, dummy=1, supported=128, best=256`
   (`mir/include/platform/mir/graphics/platform.h:159-170`). So `best+16 = 272`
   (the "good" value seen in superseded logs) and `unsupported = 0` (what we see
   now). **"Support priority: 0" therefore literally means
   `atoi(property_get("ro.build.version.sdk")) < 26`.**

2. **The property read is what fails, not the HAL stack.** The absence of any
   `[ Linking ... ]` line for gralloc/hwcomposer/libEGL/libGLESv2 is expected:
   this probe performs no dlopen at all — it returns 0 on a failed/empty
   `ro.build.version.sdk` read and Mir bails out before `create_host_platform`
   is ever reached. (The earlier same-session libEGL/libGLESv2 attempts were
   from the OLD probe, before `5831ded`, which called
   `hw_get_module(HWC_HARDWARE_MODULE_ID)`.)

3. **Most likely root cause: `ro.build.version.sdk` is not readable at probe
   time in the Ubuntu Touch host.** The real value exists (`ro.build.version.sdk=35`
   in `out/target/product/perseus/system/build.prop`), so the property is simply
   unavailable/unpopulated to the process that runs the probe, and
   `property_get` returns its `""` default → `atoi("") = 0` → probe returns 0.

   Two candidate failure paths, both credible, both unconfirmed which one binds
   first on the live device:
   - **(bionic path, most likely)** Now that libcutils.so loads (post-libdl-fix),
     the module's `::property_get` binds to bionic's implementation
     (libcutils `property_get` → `__system_property_get` → `system_properties.Get`).
     Bionic needs `PROP_DIRNAME "/dev/__properties__"` populated by
     droid-hal-init's property service. `SystemProperties::Find` returns
     `nullptr` when `!initialized_`; `__system_property_area__` stays `nullptr`
     (`bionic/libc/bionic/system_property_api.cpp:44`). On a UT host boot,
     `/dev/__properties__` is not present when the compositor starts
     (device log: `/dev/__properties__: No such file or directory`), so the read
     returns empty → 0.
   - **(libhybris fallback path)** If bionic never initializes, libhybris's
     `my_property_get` falls back to a propcache parsed from `/system/build.prop`
     and `/proc/cmdline` (`hybris/common/legacy_properties/cache.c`). This path
     would actually return `35` and produce priority 272 — i.e., if the fallback
     were active, we would NOT see priority 0. The presence of priority 0
     therefore implies the **bionic path is the one that is live and returning
     empty** (consistent with libcutils now loading after the libdl fix).

4. **Recommended next diagnostic (zero code change): force the platform module.**
   Mir 1.8.3 supports `MIR_SERVER_PLATFORM_GRAPHICS_LIB` (env) /
   `--platform-graphics-lib` (option) and `MIR_SERVER_PLATFORM_PATH` /
   `--platform-path`
   (`mir/src/platform/options/default_configuration.cpp:63-65,246-247,252-269`).
   When a platform is explicitly specified and its probe returns
   `< PlatformPriority::supported`, Mir logs
   `"Manually-specified graphics platform does not claim to support this system. Trying anyway..."`
   and proceeds to `create_host_platform` anyway
   (`mir/src/server/graphics/default_configuration.cpp:100-124`). So a live
   re-test with the module forced on bypasses the priority-0 gate and would show
   the REAL next failure (HAL load, EGL, HWC, etc.). This is the highest-value
   single experiment.

5. **Candidate fixes (in order of preference), all source-consistent:**
   - **(Rootfs/env, no code)** Set `MIR_SERVER_PLATFORM_GRAPHICS_LIB` to the
     android2 module (or `--platform-graphics-lib=...`) in the
     lomiri-system-compositor launch environment, accepting the "trying anyway"
     path, to reach `create_host_platform`. Also try `MIR_SERVER_PLATFORM_PATH`.
   - **(Environment, matches real Android)** Make bionic's property area
     available to the compositor before it probes: ensure droid-hal-init's
     property service has created `/dev/__properties__` (populated from
     `/system/build.prop`, which contains `ro.build.version.sdk=35`) and that
     the compositor runs after `/dev/socket/property_service` is up
     (`droid-hal-startup.sh` already waits for that socket at ~line 966).
   - **(Source, restore old lenient probe)** Revert the probe to the
     pre-`5831ded` behavior (return `best` when `hw_get_module(HWC_HARDWARE_MODULE_ID)`
     fails, i.e. the "Hack for Treble HWComposer 2 devices" path), or make the
     probe return a nonzero priority without depending on `ro.build.version.sdk`.
     This is a change to the installed `mir-android2-platform` package (a .deb,
     not in our libhybris trees), so it requires rebuilding/repackaging that
     package.
   - **Note on libhwc2_compat_layer.so:** per the task's own analysis this is
     probably a red herring for the probe stage (probe fails before HWC). It may
     matter AFTER the probe succeeds, at HWC2 initialization time, if the module
     reaches the "Error opening HWC HAL. Assuming HWComposer 2 device with
     libhwc2_compat_layer" path — but that string lives in the package's own
     code (`src/platforms/android/server/hwc_*.cpp` / real_hwc2_wrapper), not in
     stock libhybris. Address only if a later stage actually hits it.

---

## Q1. Is the mir-android2-platform SOURCE available anywhere?

**Not in this environment.** Nothing under `~/hadk` (checked `external/`,
`hybris/`, all `mw/` and `docs/` trees), no apt package, no `.dsc`/`.orig.tar.*`/
`.debian.tar.*`, no `/usr/src` copy, no apt archive cache copy. `dpkg -l` and
`apt-cache search` on this host return nothing mir-related.

**Upstream located (network, via gh + gitlab API):**
- **Ubports repo:** `https://gitlab.com/ubports/development/core/hybris-support/mir-android2-platform`
  — this is the exact package source (debian/control says `Source: mir-android2-platform`;
  changelog: `mir-android2-platform (1.8.0)` "Rename the package to
  mir-android2-platform", 2021-12-28). Cloned to `/tmp/opencode/mir-android2-platform`.
  - The installed `.so.15` matches this repo at commit `5831ded`:
    `git ls-tree 5831ded debian/` lists `debian/mir-platform-graphics-android2-15.install`
    (installs `graphics-android2.so.15`). Current `main` ships
    `graphics-android2.so.16` (Mir platform ABI 16). So the device's `.so.15`
    == the 1.8.0/`5831ded` era.
- **Ubports packaging for Mir 1:** `https://gitlab.com/ubports/development/core/packaging/mir1`
  (debian-only). `debian/ubports.source_location` →
  `https://github.com/canonical/mir/archive/refs/tags/v1.8.3.tar.gz`;
  `debian/watch` → `https://github.com/MirServer/mir/tags`. Mir v1.8.3 cloned to
  `/tmp/opencode/mir`.
- **Not the Mir 2.x fork:** `NotKit/mir-android2-platform` (GitHub, branch
  `personal/notkit/mir2`) and ubports `mir2` packaging target Mir 2.x — not the
  installed Mir 1.8.

## Q2. What does `probe_graphics_platform` check before HAL access?

Exact answer from source (`src/platforms/android/server/platform.cpp:358-379`):
**nothing but `ro.build.version.sdk`.** The probe calls
`::property_get("ro.build.version.sdk", propval, "")` (via
`libandroid-properties`, per `CMakeLists.txt` `ANDROID_PROPERTIES_LDFLAGS`) and
returns `best+16 = 272` iff `atoi(propval) >= 26`, else `unsupported = 0`.

Git history (`git log --oneline -S "ro.build.version.sdk"` → `5831ded`) confirms
this is a deliberate simplification. The OLD probe (pre-`5831ded`, shown by
`git show 5831ded`) did:

```cpp
int err;
hw_module_t const* hw_module;
err = hw_get_module(HWC_HARDWARE_MODULE_ID, &hw_module);
// Hack for Treble HWComposer 2 devices where loading HAL fails
if (err < 0) return mg::PlatformPriority::best;
#ifdef ANDROID_CAF
... force_caf_version() / get_android_version() / author==CodeAurora checks ...
#else
if (force_caf_version()) return unsupported;
return best;
#endif
```

So:
- Old probe: never returned 0 on this device unless `ro.build.qti_bsp.abi`
  was set (non-CAF vanilla build); returned `best` even if the HWC HAL failed to
  load. It also read `ro.build.qti_bsp.abi` / `ro.build.vanilla.abi` /
  `ro.build.version.release` via `::property_get` (same property machinery).
- New probe: property-only, single point of failure.

System-property/environment/node/exception checks enumerated by the task — all
resolved:
- **System property:** YES — `ro.build.version.sdk` is the entire check. Other
  properties (`ro.build.qti_bsp.abi`, `ro.build.version.release`,
  `ro.product.device` in `device_quirks.cpp`) only matter post-probe.
- **Device node:** NO device-node check in the probe (nothing like
  `/dev/graphics/fb0` or `/dev/dri/*` in this probe path).
- **Environment variable:** NO env-var gate inside the probe. Mir itself reads
  `MIR_SERVER_PLATFORM_GRAPHICS_LIB` / `MIR_SERVER_PLATFORM_PATH` before the
  probe (see Q3), but the probe code itself checks no env vars.
- **Silently-caught exception:** Mir's `probe_module` wraps the probe call and
  logs the result; the probe itself does not throw and does not touch
  hw_get_module, so there is no "swallowed hw_get_module error" — the missing
  `[ Linking ]` lines are because the new probe never dlopens anything, not
  because an exception was swallowed at our verbosity level.

**Unconfirmed but strongly implied:** whether `::property_get` in the deployed
module resolves to bionic (via libcutils, which now loads) or to libhybris's
`my_property_get` fallback. The observed result (0) is only consistent with the
*bionic* path being live and empty (the libhybris fallback reads
`/system/build.prop` directly and would return 35 → 272). This was not
confirmed from a live trace in this session.

## Q3. More verbose logging out of the probe / Mir?

**Probe itself: no debug output exists.** The probe is a single property read;
there is nothing to turn on inside it.

**Mir core knobs (from source):**
- `MIR_SERVER_PLATFORM_GRAPHICS_LIB` (env) / `--platform-graphics-lib`
  (option): force a specific platform module.
- `MIR_SERVER_PLATFORM_PATH` (env) / `--platform-path` (option): platform search
  dir (default `MIR_SERVER_PLATFORM_PATH`, the compiled-in
  `/usr/lib/aarch64-linux-gnu/mir/server-platform`).
  (`mir/src/platform/options/default_configuration.cpp:63-65, 246-247, 252-269`.)
- When the platform is forced and its probe returns `< supported`, Mir logs the
  *warning* `"Manually-specified graphics platform does not claim to support
  this system. Trying anyway..."` and continues to `create_host_platform`
  (`mir/src/server/graphics/default_configuration.cpp:100-124`). **This is the
  most useful "verbose" lever: it converts the priority-0 gate into a warning
  and reveals the next real failure.**
- Mir's default logger is `DumbConsoleLogger` (`default_server_configuration.cpp:225-233`),
  which prints every severity to stdout/stderr with no filtering — there is no
  Mir-side verbosity env var to raise in 1.8.3; the platform/driver log options
  come from `add_graphics_platform_options`
  (`platform.cpp:338-356`: `hwc-log`, `report-fb-native-window`,
  `disable-overlay-optimization`, plus `DeviceQuirks::add_options`), which only
  matter after the probe succeeds.

**Libhybris-side (already in use):** `HYBRIS_LD_DEBUG=1` (per-library
`[ Linking ... ]` traces) — this is what proves every library resolves. The
absence of HAL `[ Linking ]` lines at this stage is expected (see Bottom line #2).

## Q4. Bottom-line recommendation

1. **First, force the platform to bypass the probe gate and capture the real
   next failure.** In the lomiri-system-compositor launch environment set
   `MIR_SERVER_PLATFORM_GRAPHICS_LIB=/usr/lib/aarch64-linux-gnu/mir/server-platform/graphics-android2.so.15`
   (and/or pass `--platform-graphics-lib=...`; alternatively
   `MIR_SERVER_PLATFORM_PATH`/`--platform-path`). Keep `HYBRIS_LD_DEBUG=1`.
   Expect Mir to log the "Manually-specified ... Trying anyway..." warning and
   proceed; the resulting trace will show whether the failure moves past the
   probe into HAL/EGL/HWC loading. This directly distinguishes "probe gate" from
   "post-probe HAL problem" and also confirms which property path binds.
2. **Then, fix the property source.** Make `/dev/__properties__` exist and be
   populated (from `/system/build.prop`, sdk=35) before the compositor starts —
   i.e., ensure the compositor launches after droid-hal-init's property service
   is up (`/dev/socket/property_service`; droid-hal-startup.sh already waits for
   it ~line 966). If bionic properties still can't be served to the host
   process, the fallback is to make the probe not depend on the property (source
   change in the mir-android2-platform package — restore the lenient
   `hw_get_module`-based probe or return a nonzero priority unconditionally).
3. **Treat libhwc2_compat_layer.so as a later-stage issue** (only relevant after
   the probe succeeds, at HWC2 init). Verify it's actually needed only if/when a
   forced run reaches the "Error opening HWC HAL" path.

## Appendix: exact citations

- Probe (new): `mir-android2-platform/src/platforms/android/server/platform.cpp:358-379`
- Probe (old, pre-5831ded): `git show 5831ded -- src/platforms/android/server/platform.cpp`
- Rename/version commit: `5831ded` (2021-12-28); changelog `mir-android2-platform (1.8.0)`
- `.so.15` packaging at that commit: `git ls-tree 5831ded debian/` →
  `debian/mir-platform-graphics-android2-15.install`
- PlatformPriority enum: `mir/include/platform/mir/graphics/platform.h:159-170`
  (unsupported=0, dummy=1, supported=128, best=256)
- Mir throw site: `mir/src/server/graphics/platform_probe.cpp:93-120`
  ("Failed to find platform for current system" at 119)
- Mir platform selection + force/try-anyway: `mir/src/server/graphics/default_configuration.cpp:100-124`
- Mir option/env names: `mir/src/platform/options/default_configuration.cpp:63-65,246-247,252-269`
- Mir default logger: `mir/src/server/default_server_configuration.cpp:225-233`
- Platform options (post-probe logging): `platform.cpp:338-356`
- Device sdk value: `out/target/product/perseus/system/build.prop` →
  `ro.build.version.sdk=35`, `ro.build.version.release=15`
- bionic property area: `bionic/libc/include/sys/system_properties.h:184`
  (`PROP_DIRNAME "/dev/__properties__"`); `bionic/libc/system_properties/system_properties.cpp:162-174,259-268`
  (Find→nullptr when !initialized_; Get returns ""); `bionic/libc/bionic/system_property_api.cpp:44,47-49,86-88`
- libhybris libc.so properties init special path:
  `hybris/common/q/linker_soinfo.cpp:461-483` (calls `__system_properties_init`);
  evidence in `device-logs/hybris-debug.log:1030-1031`
- libhybris fallback property path: `hybris/common/legacy_properties/properties.c:165-204`
  (`my_property_get`), `hybris/common/legacy_properties/cache.c:67-276`
  (propcache from `/system/build.prop` + `/proc/cmdline`);
  `hybris/common/hooks.c:2341` (property_get→my_property_get);
  `hybris/properties/hybris_properties.c` (dlsym bionic via libcutils + my_* fallback)
- `/dev/__properties__` handling: `hybris/droid-configs/sparse/usr/bin/droid/droid-hal-startup.sh:504-506`
  (remove stale), ~966 (wait for `/dev/socket/property_service`);
  device log `device-logs/fresh-test10/droid-hal-debug.log` line
  `11:07:47:21 startup: /dev/__properties__: ls: /dev/__properties__: No such file or directory`
- Prior-task context (libdl bootstrap skip, from LIBDL_SO_RESEARCH_FINDINGS.md):
  `hybris/mw/libhybris/libhybris/hybris/common/q/linker.cpp:1155-1161`

## Appendix: unconfirmed / not fully checked

- Which `property_get` implementation (bionic-vs-libhybris-fallback) actually
  binds in the deployed module's probe call — inferred (priority 0 implies
  bionic-empty path), not observed in a live trace this session.
- Whether `/dev/__properties__` is populated by droid-hal-init before
  lomiri-system-compositor starts on the current UT boot (only a startup-time
  "No such file or directory" log was available).
- Live re-test with `MIR_SERVER_PLATFORM_GRAPHICS_LIB` forced was not performed
  (research-only session; no device access).
- Exact behavior of the q linker's bionic `__system_properties_init` when the
  property area directory is absent (would return empty in
  `SystemProperties::Get`; not live-verified).
