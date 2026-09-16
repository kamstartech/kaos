# Session Handoff — 2026-09-16 (Kaos overlay mode / phosh-app)

This documents one continuous work session covering: the Kaos overlay-mode display pipeline,
a Launcher "-1 screen" plugin sketch, Android-notification forwarding into the Kaos desktop
(now confirmed working end-to-end in Java, but invisible on screen — root-caused), a real
namespace-lifecycle bug found and repeatedly hand-mitigated (never fixed in code), a
framework-level fix for the hardware AI-button escape hatch (confirmed working), and two full
`make systemimage` build-and-flash cycles. Not a git repo (`git` unavailable in this tree), so
there is no commit history to point to — everything below is direct file state as of session
end, verified live on-device where noted.

**Read this before touching `kaos/apps/phosh-app/`, `kaos/scripts/kaos-display-bridge.cpp`, or
`frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java` again**
— several of the changes below only make sense together.

---

## 1. What's new and working (built, flashed, verified live)

### 1a. Event-based display link (`KaosDisplayLink`) — replaces one-shot show/hide

**Problem it fixes:** `PhoshDisplayActivity` used to fire a single fire-and-forget `CMD_SHOW`
over a DGRAM socket and assume success. If `kaos-display-bridge` (or the whole namespace)
wasn't running, the activity still set `surfaceVisible=true`, leaving a transparent,
all-touch-consuming, dead overlay on screen with no way out except the hardware AI button.
This is what "opening the app looks broken" meant.

**New design:**
- `kaos/scripts/kaos-display-bridge.cpp` — added a second listening socket,
  `LINK_SOCKET_PATH = /dev/socket/kaos_display_link.sock` (`SOCK_STREAM`, one client at a
  time), alongside the pre-existing `CTL_SOCKET_PATH` (`SOCK_DGRAM`, unchanged). Show/hide
  command handling was factored into a shared `handle_link_cmd` lambda used by both sockets.
  **New behavior:** if the link client disconnects for any reason (crash, kill, explicit
  close), the daemon auto-hides — see the `link_client_idx` poll branch near the end of the
  main loop.
- `kaos/apps/phosh-app/src/com/kaos/phosh/KaosDisplayLink.java` (new) — pure Java
  (`android.net.LocalSocket`, no JNI). `connect()` retries in the background until the daemon
  actually accepts (the "wait for something to connect" behavior the user asked for); a
  blocking `read()` on the same connection unblocks the instant the peer closes, which is the
  disconnect event, delivered by the kernel with zero polling once connected.
- `PhoshDisplayActivity.java` — `onResume()` just calls `displayLink.connect()` and returns.
  `surfaceVisible` stays `false` (touches keep passing through, no dead-overlay risk) until
  `onConnected()` actually fires. `onDisconnected()` (only fires for a genuine peer-initiated
  close, not our own `onPause()`/`stop()`) toasts "Kaos desktop disconnected" and `finish()`es.
- `native-lib.cpp` — removed the four now-dead `PhoshDisplayActivity` JNI exports
  (`nativeInit`/`nativeShowSurface`/`nativeHideSurface`/`nativeDestroy`) and the unused
  `CMD_QUIT` constant. **`CTL_SOCKET_PATH` (DGRAM) is still alive** — it's now used
  exclusively by the Launcher plugin (`KaosOverlayNative`, see below), which doesn't need
  live-disconnect detection yet.

**Verified live (2026-09-16):** launched `PhoshDisplayActivity` directly via
`adb shell am start -n com.kaos.phosh/.PhoshDisplayActivity` with the namespace stopped — no
crash, app's own debug log (`/data/data/com.kaos.phosh/files/phosh-debug.log`) shows
`onResume, connecting KaosDisplayLink` and then silently retries in the background, exactly as
designed. Also verified connected with the namespace actually running (§1d below covers the
escape-hatch half of the loop). **Still not exercised: a live `kaos-display-bridge` crash/kill
while connected**, to confirm `onDisconnected()` fires correctly for a genuine mid-session
disconnect rather than just "never connects."

### 1d. Hardware AI-button escape hatch — root-caused and fixed (framework-level)

**Problem:** the only documented way to leave the Kaos overlay besides losing all touch/Home/
Recents access was the hardware AI button (`KEYCODE_VOICE_ASSIST`), and the app-side handler for
it (`PhoshDisplayActivity.onKeyDown()`) had been sitting there for a while marked "unverified."
Live testing this session — both a real physical button press and a synthetic
`adb shell input keyevent 231` — confirmed it did nothing at all: `sys.kaos.display_shown`
never changed, no new line in the app's debug log, meaning the key never reached the app.

**Root cause:** Android 15's own internal key-gesture refactor conflicts with the existing Kaos
patch. The Kaos-aware code lives in `PhoneWindowManager.interceptKeyBeforeQueueing()`
(`frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java:~6335`,
tracks `mKaosVoiceAssistDownTime`) and correctly decides to pass `KEYCODE_VOICE_ASSIST` through
to the foreground app when `sys.kaos.display_shown=1`. But a *separate*, later interception
point — `interceptKeyBeforeDispatching()` → `interceptSystemKeysAndShortcuts()` → (depending on
the `useKeyGestureEventHandler()` flag) either `interceptSystemKeysAndShortcutsOld/New` in the
same file, or `KeyGestureController.java` — has its own `case KeyEvent.KEYCODE_VOICE_ASSIST`
that unconditionally consumes the key with a `Slog.wtf(..., "should be handled in
interceptKeyBeforeQueueing")` stub. Despite that log message's own claim, this stub runs *after*
queueing and overrides its decision, since queueing and dispatching are independent interception
points and stock AOSP has zero Kaos awareness at the dispatching stage. Confirmed via
`strings`-diffing `/system/framework/services.jar` pulled from the device against the local
build output (checksums matched — this wasn't a stale-build issue, the conflicting stock code
was genuinely what's running).

**Fix (`PhoneWindowManager.java`, `interceptKeyBeforeDispatching`, right before the
`interceptSystemKeysAndShortcuts()` call site around line 3936):** a single guard —
`kaosClaimedThisKey = ACTION_DOWN && keyCode == KEYCODE_VOICE_ASSIST && mKaosVoiceAssistDownTime
== event.getDownTime()` — skips the call entirely for exactly the down event Kaos already
claimed. This is one fix point covering every downstream consumer (both old/new paths *and*
`KeyGestureController`) instead of patching each WTF stub separately; behavior for every other
key/app is byte-for-byte unchanged. The UP event doesn't need the same guard — by the time it
reaches dispatching, `mKaosVoiceAssistDownTime` has already been reset to `-1` by the existing
queueing-stage logic, and letting the UP fall through to the stock stub is harmless since the
app only acts on DOWN. Also removed the temporary `KaosVoiceAssistProbe` diagnostic (logged
every key event) from `interceptKeyBeforeQueueing`, per its own "remove once concluded" comment.

**Verified live (2026-09-16), after a full `make systemimage` rebuild + reflash:** pressed the
real hardware AI button while the desktop was shown — debug log immediately logged
`"AI button (VOICE_ASSIST) pressed — hiding Kaos overlay"`, `sys.kaos.display_shown` flipped
`1→0`, and the very next touch events showed `surfaceVisible=false`. Fully working, both
real-hardware and synthetic (`input keyevent 231`) triggers confirmed. The "unverified" status
on this escape hatch is resolved.

### 1b. Launcher "-1 screen" plugin sketch (not wired into Trebuchet's runtime, sketch only)

Implements the real, documented AOSP SystemUI/Launcher plugin mechanism (`LauncherOverlayPlugin`
via `PluginManagerImpl`, signature-permission-gated, no core Trebuchet fork needed).

- Vendored (copied, per the AOSP-documented pattern of each plugin author vendoring their own
  copy) into `kaos/apps/phosh-app/src/com/android/systemui/plugins/`:
  `Plugin.java` (trimmed of the deprecated `getVersion()`/`@ProtectedReturn` machinery, which
  doesn't exist anywhere in this tree), `annotations/ProvidesInterface.java`,
  `LauncherOverlayPlugin.java`, `shared/LauncherOverlayManager.java`.
- New, under `kaos/apps/phosh-app/src/com/kaos/phosh/launcheroverlay/`:
  - `KaosLauncherOverlayPlugin.java` — the plugin entry point (`<service>`-discovered).
  - `KaosOverlayManager.java` — implements `LauncherOverlayManager.openOverlay()`/
    `hideOverlay()`, forwarding to native show/hide. **Does not yet implement**
    `LauncherOverlayTouchProxy` — show/hide fire on the discrete open/close transition only,
    not proportionally during the swipe (no "track the finger" behavior like Google Feed).
  - `KaosOverlayNative.java` — JNI declarations bound to `native-lib.cpp`'s
    `Java_com_kaos_phosh_launcheroverlay_KaosOverlayNative_nativeShow/nativeHide`.
- `AndroidManifest.xml` — added `<uses-permission android:name="com.android.systemui.permission.PLUGIN" />`
  and a `<service android:name=".launcheroverlay.KaosLauncherOverlayPlugin" android:exported="false">`
  with the `PLUGIN_LAUNCHER_OVERLAY` intent-filter action, matching
  `frameworks/base/packages/SystemUI/plugin/ExamplePlugin`'s manifest pattern exactly (verified
  by reading that file directly).

**Verified:** the service is correctly discoverable on-device
(`dumpsys package com.kaos.phosh` → Service Resolver Table shows
`com.kaos.phosh/.launcheroverlay.KaosLauncherOverlayPlugin`). **Never actually tested inside
Trebuchet** — nobody has swiped to the `-1` screen and confirmed Launcher picks this plugin up,
binds it, and calls `createOverlayManager()`. That's real, unstarted integration testing.

### 1c. Android-notification forwarding into the Kaos desktop

**Why:** `kaos-display-bridge`'s SF surface is a hardcoded `setLayer(1000000)` — above every
normal Android layer including the status bar/notification shade — so Android notifications
are fully invisible whenever the desktop is shown, with no existing forwarding path. This adds
one, gated behind a user toggle (per explicit request: "so we can choose if we want to see
them or not").

- `KaosNotificationListener.java` (new) — a `NotificationListenerService`. Filters out the
  app's own notifications, checks a `phosh_prefs`/`forward_notifications` boolean (default
  **off**), forwards title/text/small icon via a static in-process bridge.
- `KaosNotificationBridge.java` (new) — trivial static listener registry, no cross-process
  IPC needed since both run in `com.kaos.phosh`'s own process.
- `PhoshDisplayActivity.java` — registers itself as the bridge listener in `onConnected()`
  (moved from the old `onResume()`), shows a small banner (icon+title+text,
  `res/layout/comp_notification_banner.xml`) that auto-dismisses after 4s. Deliberately **not
  tappable** — `dispatchTouchEvent()` still swallows all touches while the desktop is up, so
  this is display-only by design, not a full notification center.
- `MainActivity.java` / `activity_main.xml` — added a "Show notifications on Kaos desktop"
  `Switch`, persisted in `phosh_prefs`. Turning it on also launches
  `Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS` if listener access isn't granted yet —
  that's a one-time user-granted special permission Android requires regardless of platform
  signing; it cannot be silently self-granted.
- `AndroidManifest.xml` — registered `KaosNotificationListener` with
  `BIND_NOTIFICATION_LISTENER_SERVICE`.

**Verified:** service correctly discoverable via `dumpsys package` (Service Resolver Table), and
**the whole Java pipeline is confirmed working end-to-end** — after adding stage-by-stage
`KaosDebugLog`/`Log.i` instrumentation (see `KaosDebugLog.java`, new) to
`KaosNotificationListener`, `KaosNotificationBridge`, and `PhoshDisplayActivity`, a real
Telegram notification ("Bruce") was captured live in the debug log all the way through:
`onNotificationPosted` → `forwardingEnabled=true` → `dispatching` → `KaosNotificationBridge
.dispatchPosted` (listener non-null) → `PhoshDisplayActivity.onNotification` →
`showNotificationBanner` → `visibility=0` (`View.VISIBLE`). The toggle, the listener-access
grant, and the forwarding code all work correctly.

**But the banner is still never visible on screen — root-caused, not yet fixed.** Checked
`adb shell dumpsys SurfaceFlinger --list` directly: `KaosDesktop` (kaos-display-bridge's raw
compositor surface) sits at `z=1000000`; `PhoshDisplayActivity`'s own window layer sits at
`z=1` (WindowManager's normal default for an app window — nothing in the app can move it above
an arbitrary raw value like `1000000` through any public API). SurfaceFlinger draws higher z on
top, so **`PhoshDisplayActivity`'s entire window — including the notification banner — has been
rendering underneath the opaque Kaos desktop this whole time**, regardless of whether the Java
code runs correctly. This also explains, retroactively, why touch-consumption still works even
though the window is invisible: input focus (which window gets touches) and SurfaceFlinger
z-order (what's visually on top) are separate subsystems, and `PhoshDisplayActivity` holds input
focus without holding the top paint position.

An earlier, disproven theory (worth noting so it isn't retried): a Phosh-rendered "Slide up to
unlock" screen was briefly mistaken for evidence that Android windows *can* draw over
`KaosDesktop`. That screen was actually Phosh's *own* lock UI, composited inside the same
`KaosDesktop` surface — not a competing Android window at all. Ignore that as evidence either
way.

**Real fix options, none implemented yet** (discussed but intentionally not started, see §2b
and the Launcher-roadmap note in §6): render the banner inside `kaos-display-bridge` itself
(app sends rendered pixels over the socket, daemon blits into the surface it already owns), or
give the banner its own raw SurfaceFlinger layer created above `1000000` directly via
`SurfaceComposerClient` (native, parallel to how `KaosDesktop` itself is created). Leaning
toward the former to avoid multiple raw layers needing coordination.

---

## 2. Real bugs found — needs code fixes

### 2a. In-namespace `poweroff` orphans Android-side daemons

**What happens today:** running `poweroff`/`shutdown -h now`/Phosh's power-menu *inside* the
Ubuntu namespace does **not** reboot or power off the physical device — `kaos-service` launches
the namespace via `unshare_kaos -m -p -u -i -C -f` (kaos-service:1071, `-p` = `CLONE_NEWPID`
present in both the primary path and the `-m -p -f` degraded fallback), and since Linux 3.4,
`reboot(2)` called from a non-initial PID namespace is intercepted by the kernel and converted
into killing that namespace's PID 1 instead of touching the real hardware. **The kernel
protection works.**

**What's actually broken:** `kaos-service` has **no supervisor** watching for the namespace's
own PID 1 exiting on its own (as opposed to being killed by an explicit `kaos-service stop`).
`start_namespace()` writes `$MAIN_PID` to a PID file and returns; only `stop_namespace()`
(only reached via an explicit `stop` command) ever cleans up `kaos-touch-input` and
`kaos-display-bridge`. So after an in-namespace poweroff:
- `kaos-touch-input` and `kaos-display-bridge` (both Android-side, root, PID-tracked in
  `/data/local/tmp/kaos-{input,ubuntu,display-bridge}.pid`) keep running, orphaned under host
  PID 1.
- `kaos-service status`/`kaos-starter status` keep reporting "running" off the stale PID file.
- If the desktop was visible, the screen goes black/frozen (Phoc died, nothing's compositing)
  with zero user-facing signal — this is exactly why (1a)'s `KaosDisplayLink` disconnect event
  matters: once the namespace side actually gets fixed, `KaosDisplayLink` will correctly
  surface this via `onDisconnected()`, but until then there's no automatic app-side recovery
  either, since a plain crash of `kaos-display-bridge` was never exercised as a live test this
  session (only "kaos-display-bridge isn't running at connect time" was tested).

**Confirmed this happened at least three times across the session** (`/data/adb/kaos-service.log`):
first, a `stop` at `22:10:07` had no `Killing namespace PID 1 (host PID ...)` line — meaning the
real PID 1 was *already dead* before anyone ran `stop` — versus an earlier, clean stop at
`17:19:45` that did log killing a live PID 1. It then reproduced again later the same session
(fresh `kaos-touch-input`/`kaos-display-bridge` orphans under a new namespace start), confirming
this is reliably reproducible on demand, not a one-off.

**Mitigation done this session (manual, not code, done twice):** used `adb root` (not `su` —
confirmed `su: inaccessible or not found` on this build; the real root path is either the
port-30000 bridge or `adb root`) to `kill -9`/`kill -TERM` the orphaned daemons and remove the
stale PID files each time. `kaos-service --distro ubuntu status` correctly reports `Inactive`
again after cleanup. **Still no code fix** — every occurrence this session was cleaned up by
hand.

**Not done — real next step:** add an actual monitor to `kaos-service`'s `start_namespace()`,
e.g. a background `wait $MAIN_PID` (or equivalent watcher) that runs the same cleanup
`stop_namespace()` does the moment the namespace's PID 1 exits on its own, regardless of cause.
No code was written for this — it's diagnosed and reasoned through, not fixed. Given it's now
reproduced multiple times and requires a manual adb cleanup each time, this is the single
highest-value fix left in the whole project.

### 2b. Notification banner z-index occlusion (see §1c) — real fix not implemented

Covered in full under §1c above: `kaos-display-bridge`'s `KaosDesktop` SF surface at `z=1000000`
draws over `PhoshDisplayActivity`'s own window (`z=1`), so the notification banner is fully
functional in Java but never visible. Two fix directions were discussed (render into
`kaos-display-bridge`'s own surface vs. give the banner its own raw layer above `1000000`), but
given the Launcher-roadmap discussion in §6, this may be better addressed as part of that
migration rather than patched standalone in `PhoshDisplayActivity` — see §6 for the tradeoff.

---

## 3. Build fixes (both required for `make systemimage`/`make PhoshApp` to succeed)

1. **Invalid XML comments in `AndroidManifest.xml`.** XML forbids a bare `--` anywhere inside
   a `<!-- -->` comment except as the closing delimiter. Introduced this session (twice — once
   in the PLUGIN-permission comment, once in my own fix-explanation comment for bug #2 below)
   by writing prose asides like `plugin --\n signature-level...`. Fixed by switching to em
   dashes (`—`). **If you add more manifest comments, don't use `--` as a prose dash.**
2. **Lint `Instantiatable` false positive** on `KaosLauncherOverlayPlugin`'s `<service>` entry.
   Android Lint assumes every `<service>` is a real `android.app.Service`; this one is the
   standard AOSP plugin-discovery pattern (a plain class instantiated via reflection by
   `PluginManagerImpl`, never bound/started as a Service) — exactly what `ExamplePlugin`'s own
   manifest does, which never hits this check because AOSP builds it via Soong, not Gradle
   lint. Suppressed with `tools:ignore="Instantiatable"` (added `xmlns:tools` to the manifest
   root). This is a real, intentional suppression, not a hack to revisit.

Both `make PhoshApp` (standalone) and the full `make systemimage` succeeded after these two
fixes (`#### build completed successfully (41:16) ####`).

## 4. Deployment gotcha discovered — will bite again if not remembered

**Flashing `system.img` does not remove a shadowing `/data/app` update.** After flashing the
freshly built `system.img` (`fastboot flash system out/target/product/perseus/system.img`),
`com.kaos.phosh` kept running an **18-day-stale** build: `dumpsys package com.kaos.phosh`
showed `pkgFlags=[... UPDATED_SYSTEM_APP ...]` with `codePath=/data/app/~~.../com.kaos.phosh-.../`
— a leftover from an earlier `mka`/`adb install`-style push this session that landed in `/data`
and fully shadows anything in `/system/priv-app/`, since `/data` is untouched by a raw
partition flash. Confirmed via checksum: the `/data/app` copy was 17.2MB dated Sept 6; the
freshly flashed `/system/priv-app/PhoshApp/PhoshApp.apk` was 18.0MB dated today and *did*
match the local build output exactly — the flash worked, it just wasn't visible.

**Fix:** `adb uninstall com.kaos.phosh` (equivalent to Settings → App Info → "Uninstall
updates") — removes the `/data/app` shadow, falls back to the `/system/priv-app` version.
Confirmed after: `codePath` correctly points at `/system/priv-app/PhoshApp`, no more
`UPDATED_SYSTEM_APP` flag, and the new manifest's permissions/services (PLUGIN permission,
both new services) show up for the first time.

**Rule of thumb for next time:** after any `fastboot flash system`, if a priv-app was ever
side-loaded/`mka`-installed onto this device before, run `adb uninstall <pkg>` for it before
trusting that the new build is what's actually running. `dumpsys package <pkg>` and check for
`UPDATED_SYSTEM_APP` in `pkgFlags` is the diagnostic.

**Smaller gotcha, same family:** right after a fresh `/data`-wiping flash+first-boot,
`am start -n com.kaos.phosh/.PhoshDisplayActivity` can fail once with `Error: Activity class
{...} does not exist`, even though `pm list packages` already shows the package and
`sys.boot_completed=1`. Transient — PackageManagerService is still finishing its first-boot
component scan. Just wait a bit and retry; it resolved on the second attempt with no other
action needed.

---

## 5. Access notes for the next session

- **SSH is currently down** (`ssh phosh@<device-ip>` → connection refused) because `sshd` runs
  *inside* the Ubuntu namespace, which is stopped (see §2a). It'll come back once the namespace
  is started again. Don't assume SSH access without checking this first.
- **`su` does not exist** on this build (`su: inaccessible or not found`). Root is either the
  port-30000 bridge (`echo '<cmd>' | nc 127.0.0.1 30000`, works even with the namespace down)
  or `adb root` (works, but doesn't persist across device reboots — re-run after every
  reboot/flash).
- Namespace state as of session end: **`kaos-service --distro ubuntu status` → `Inactive`**
  (stopped cleanly via `kaos-service --distro ubuntu stop` — logged `Killing namespace PID 1`,
  no orphans left behind, unlike §2a's failure mode). Starting it again is the natural first
  step for any further live testing of §1a/§1b/§1c.
- `hybris/mw/mesa-freedreno/rpm/` is SailfishOS-only — explicit standing instruction, never
  touch it regardless of task.
- Build ownership: the user builds this project themselves by default; only build when
  explicitly asked (a direct request, or a `/goal` invocation scoped to a build task like the
  ones that produced §3/§4 and §1d above — this session had two separate full
  `make systemimage` build-and-flash cycles, both user-initiated).
- A full `frameworks/base` change (like §1d's fix) needs a full `make systemimage` rebuild —
  confirmed this works cleanly end to end, twice, this session. Don't assume a
  `frameworks/base`/`services.jar` edit is live until that's been done; `adb push`ing a leaf
  module doesn't touch it.

---

## 6. Suggested next steps, roughly in priority order

1. **Fix §2a** — add a namespace-PID-1 monitor to `kaos-service`. Reproduced multiple times
   this session, always needing manual `adb root` cleanup; the single highest-value remaining
   fix.
2. **Decide on the z-index fix (§1c/§2b) vs. the Launcher-integration roadmap.** These pull in
   different directions: `KaosOverlayManager` (§1b) is architecturally *already* closer to
   solving the notification-occlusion problem for free — it never consumes Launcher's own
   touches or hides system UI the way `PhoshDisplayActivity` does, so a real `-1`-screen
   experience would likely coexist with the status bar/notifications naturally, the way
   Google's Discover feed page does. But both paths currently share the exact same
   `kaos-display-bridge` `z=1000000` surface, so wiring up the Launcher plugin as-is would
   inherit the identical occlusion — the z fix isn't optional, just where it should live.
   Patching `PhoshDisplayActivity`'s banner today is more throwaway if the Launcher path is
   truly where this is headed. Needs a decision, not just implementation.
3. Actually swipe to the `-1` screen on Launcher with this build installed and confirm §1b's
   plugin gets picked up by `PluginManagerImpl` at all — this has never been exercised, and
   nothing this session moved it forward (deliberately deprioritized in favor of §1d/§2a work).
4. Do a full live pass of §1a's remaining untested half: kill `kaos-display-bridge` by hand
   while connected and confirm `onDisconnected()` fires correctly and the activity finishes
   cleanly (connect → show → namespace-dies → auto-disconnect was never exercised, only
   connect → show → and connect-retry-while-nothing's-running).
5. Revisit the still-unresolved kernel `s_umount` deadlock from earlier in this project's
   history (`fs/proc/root.c`'s `mount_ns()` conversion) — `CONFIG_PROVE_LOCKING` was enabled
   for this but no lockdep report has ever been captured.
