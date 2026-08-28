# Null composer HAL for the perseus LXC container

## What this is

A drop-in replacement for the real vendor composer HAL inside the Halium-style
Android container.  It registers as the same HIDL service the stock vendor HAL
provides:

```
android.hardware.graphics.composer@2.3::IComposer/default
service name: vendor.hwcomposer-2-3
```

It reports exactly one fake physical display and accepts the full HWC2 command
surface, but never opens `/dev/dri/card0`, `/dev/dri/renderD128`,
`/dev/kgsl-3d0`, or any other DRM/display device.  This keeps the container's
`surfaceflinger` alive and providing binder services without taking over the
physical screen.

Default panel parameters (1080x2340 @ ~60 Hz, 403 ppi) match the Mi Mix 3
Samsung AMOLED panel.  They are configurable at build time via the
`KAOS_HWC_NULL_*` preprocessor defines in `NullComposerHal.h`; no per-device
override wiring exists yet because the project currently supports only one
device.  When a second device is added, its device tree should pass the
appropriate `-DKAOS_HWC_NULL_*` cflags (e.g. through a `cc_defaults` module or
`device.mk`) instead of hardcoding new constants here.

## Files

- `NullComposerHal.{h,cpp}` — no-op `ComposerHal` backend.
- `service.cpp` — HIDL service entry point.
- `Android.bp` — builds `android.hardware.graphics.composer@2.3-service-null`.
- `android.hardware.graphics.composer@2.3-service-null.rc` — service definition
  (intentionally **not** installed to `/system/etc/init/`; it is bind-mounted
  over the real vendor RC inside the container only).

## Build integration

Source lives here (`kaos/hal/hwcomposer-null/`), deliberately outside any
device tree -- this is OS/Halium architecture, not perseus-specific.  Default
panel constants are tuned for the Mi Mix 3 Samsung AMOLED panel but are
exposed as preprocessor defines so they can be overridden from a future
device tree without forking this generic module. (`kaos/` is the project's
rebrand from "HybridOS" -- see
`kaos/Android.bp`'s own header comment.) `kaos/Android.bp` declares the
`soong_namespace{}` + `package{ default_applicable_licenses:
["Android-Apache-2.0"] }` this directory needs (the public AOSP Apache-2.0
license module -- NOT `hardware_interfaces_license`, which is scoped to
`hardware/interfaces/` itself and is not visible to an external tree;
confirmed via a real build failure before this was fixed).
`device/xiaomi/perseus/device.mk` adds `kaos` to `PRODUCT_SOONG_NAMESPACES`
so the module resolves from the device build.

`kaos/kaos.mk` adds:

```makefile
PRODUCT_PACKAGES += android.hardware.graphics.composer@2.3-service-null
PRODUCT_COPY_FILES += \
    kaos/hal/hwcomposer-null/android.hardware.graphics.composer@2.3-service-null.rc:\
    $(TARGET_COPY_OUT_SYSTEM)/etc/kaos/hwcomposer-null.rc
```

The binary installs to `/system/bin/hw/android.hardware.graphics.composer@2.3-service-null`.

A `file_contexts` entry in `device/xiaomi/perseus/sepolicy/vendor/file_contexts`
labels the binary `hal_graphics_composer_default_exec` (a standard, pre-existing
AOSP type -- also used by AOSP's own `composer3-service.example`) so it can
transition into the normal composer domain when SELinux is enforcing.  (The
current build runs `androidboot.selinux=permissive`, so this is defensive.)

## LXC wiring (applied)

The shadow-RC mount is injected before `droid-hal-init` parses vendor RC
files, in the mount hook in
`kaos/apps/phosh-app/src/com/kaos/phosh/SetupManager.java`,
immediately after the `keymaster-4-0.rc` shadow block and before the
`/system/etc/init/hw/init.rc` shadow block:

```java
                                + "cat > \"${LXC_ROOTFS_MOUNT}/tmp/hwcomposer-null.rc\" << 'RCEOF'\n"
                                + "service vendor.hwcomposer-2-3 /system/bin/hw/android.hardware.graphics.composer@2.3-service-null\n"
                                + "    interface android.hardware.graphics.composer@2.3::IComposer default\n"
                                + "    class hal animation\n"
                                + "    user system\n"
                                + "    group graphics drmrpc\n"
                                + "    capabilities SYS_NICE\n"
                                + "    onrestart restart surfaceflinger\n"
                                + "    task_profiles ServiceCapacityLow\n"
                                + "RCEOF\n"
                                + "mount --bind \"${LXC_ROOTFS_MOUNT}/tmp/hwcomposer-null.rc\" \"${LXC_ROOTFS_MOUNT}/vendor/etc/init/android.hardware.graphics.composer@2.3-service.rc\" 2>/dev/null\n"
```

## Why HIDL 2.3 and not AIDL Composer3

The device's real VINTF manifest declares the composer as HIDL 2.3:

```xml
<hal format="hidl">
    <name>android.hardware.graphics.composer</name>
    <transport>hwbinder</transport>
    <fqname>@2.3::IComposer/default</fqname>
</hal>
```

`SurfaceFlinger::Composer::create()` (frameworks/native/services/surfaceflinger/
DisplayHardware/ComposerHal.cpp) only uses the AIDL composer if
`AServiceManager_isDeclared(android.hardware.graphics.composer3.IComposer/default)`
is true.  Because the manifest contains no AIDL composer3 entry, SurfaceFlinger
falls back to the HIDL path.  Therefore a HIDL 2.3 stub is the only drop-in
replacement that will actually intercept the real vendor service.

## Risks / things to verify

- The stub returns `-1` for all present/release fences.  SurfaceFlinger treats
  this as "already complete"; if any path requires a real signaled sync fence
  fd, composition may stall.  The spec allows `-1` for already-complete fences.
- `getDisplayIdentificationData` returns `UNSUPPORTED`.  SurfaceFlinger falls
  back to legacy single-display mode for the primary panel, which is correct
  for this use case.
- A software vsync thread fires at ~60 Hz while `setVsyncEnabled(ENABLE)` is
  active.  If SurfaceFlinger expects a different phase or timestamp source, the
  composition pacing may be slightly off; it should still remain functional.
- The binary is installed to the system image, so it is present on a normal
  Android boot.  It is **not** started because its RC is only in
  `/system/etc/kaos/` and is never imported by native init.  Make sure no
  future change imports that RC globally.
