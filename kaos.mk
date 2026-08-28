# Kaos Configuration (device-agnostic)
# System-level integration for unrestricted property access.
# Per-device properties (e.g. ro.kaos.device) should be set by the device tree
# that inherits this file.

# Kaos Properties
PRODUCT_PROPERTY_OVERRIDES += \
    ro.kaos.version=2026 \
    ro.kaos.kernel=enabled \
    ro.kaos.preinstalled=1 \
    ro.secure=1 \
    ro.adb.secure=1 \
    ro.debuggable=0

# Kaos Scripts → /system/bin/
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/scripts/kaos-bridge:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-bridge \
    $(LOCAL_PATH)/scripts/kaos-service:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-service \
    $(LOCAL_PATH)/scripts/kaos-starter:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-starter \
    $(LOCAL_PATH)/scripts/kaos-initializer:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-initializer \
    $(LOCAL_PATH)/scripts/kaos-chroot-install:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-chroot-install \
    $(LOCAL_PATH)/scripts/kaos-installer:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-installer \
    $(LOCAL_PATH)/scripts/kaos-ssh:$(TARGET_COPY_OUT_SYSTEM)/bin/kaos-ssh \
    $(LOCAL_PATH)/scripts/distros-lib.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/distros-lib.sh \
    $(LOCAL_PATH)/scripts/distros.conf:$(TARGET_COPY_OUT_SYSTEM)/etc/distros.conf \
    $(LOCAL_PATH)/scripts/kali-preinit:$(TARGET_COPY_OUT_SYSTEM)/bin/kali-preinit \
    $(LOCAL_PATH)/scripts/kali-compat-init:$(TARGET_COPY_OUT_SYSTEM)/bin/kali-compat-init \
    $(LOCAL_PATH)/scripts/kali-hybridos-prepare:$(TARGET_COPY_OUT_SYSTEM)/bin/kali-hybridos-prepare \
    $(LOCAL_PATH)/scripts/kali-hybridos-verify:$(TARGET_COPY_OUT_SYSTEM)/bin/kali-hybridos-verify \
    $(LOCAL_PATH)/scripts/kali-hybridos.target:$(TARGET_COPY_OUT_SYSTEM)/etc/kaos/systemd/kali-hybridos.target \
    $(LOCAL_PATH)/scripts/kali-hybridos-prepare.service:$(TARGET_COPY_OUT_SYSTEM)/etc/kaos/systemd/kali-hybridos-prepare.service \
    $(LOCAL_PATH)/scripts/kali-hybridos-verify.service:$(TARGET_COPY_OUT_SYSTEM)/etc/kaos/systemd/kali-hybridos-verify.service \
    $(LOCAL_PATH)/scripts/sailfish-fixup.sh:$(TARGET_COPY_OUT_SYSTEM)/bin/sailfish-fixup.sh

# Kaos native binaries built from source in scripts/Android.mk
PRODUCT_PACKAGES += \
    kaos \
    kaos-touch-input \
    unshare_kaos \
    kaos-display-bridge \
    kaos-display-ctl

# LibHybris compat layers (external/libhybris/compat/*/Android.mk). All six
# build fine into out/target/product/perseus/system/lib64/, but only
# libsf_compat_layer.so was ever actually reaching the flashed system.img --
# confirmed live (2026-08-20) that Droidian's phoc crash-loops on
# "library 'libhwc2_compat_layer.so' not found" (and libminisf.so, tracked
# separately -- 32-bit-only build, no PRODUCT_PACKAGES entry helps that one)
# because nothing here ever declared the other five as PRODUCT_PACKAGES;
# they were only ever pulled into out/ as build intermediates, never
# packaged onto the device. libsf_compat_layer presumably only made it on
# because something else already in PRODUCT_PACKAGES links against it as a
# transitive shared-library dependency -- explicit entries here don't rely
# on that for the rest.
PRODUCT_PACKAGES += \
    libhwc2_compat_layer \
    libui_compat_layer \
    libis_compat_layer \
    libcamera_compat_layer \
    libmedia_compat_layer

# android.frameworks.vr.composer@1.0 -- transitive dependency of
# libhwc2_compat_layer, same missing-PRODUCT_PACKAGES gap as above. Also
# built fine into out/, never packaged. Confirmed live (2026-08-20): once
# libhwc2_compat_layer itself was pushed and loading, phoc's crash changed
# from a clean exit(1) ("library not found", handled gracefully) to a real
# SIGSEGV a few seconds in -- consistent with code that assumes this
# dependency loaded successfully then dereferencing something unset when
# it silently didn't.
PRODUCT_PACKAGES += \
    android.frameworks.vr.composer@1.0

# libminisf (external/droidmedia/Android.mk) -- phoc's hwcomposer backend
# needs this to provide a fake SurfaceFlinger service before it can "get"
# a hwcomposer service at all; without it, confirmed live (2026-08-20)
# phoc logs "failed to get hwcomposer service" and aborts (SIGABRT) a few
# seconds later, every retry. Only ever built for 32-bit (obj_arm, no
# obj_arm64 output in out/) despite carrying no LOCAL_32_BIT_ONLY of its
# own (unlike its neighbours minimediaservice/minisfservice in the same
# Android.mk, which do) -- consistent with it never being requested by
# anything in the 64-bit PRODUCT_PACKAGES dependency graph, so Soong never
# generated that variant. Unlike every other compat-layer fix above, this
# one has no existing arm64 .so to push directly -- needs a real rebuild
# before it can be tested.
PRODUCT_PACKAGES += \
    libminisf

# resourcemanager_aidl_interface-ndk -- transitive dependency of libminisf
# itself. Already built and staged into out/ (pulled in incidentally by
# something else already in PRODUCT_PACKAGES), just never actually
# deployed to the live device before now -- confirmed live (2026-08-20)
# libminisf still logged "incompatible or missing" even once present,
# because of this specific "library not found". Explicit entry here so
# packaging doesn't depend on staying incidental.
PRODUCT_PACKAGES += \
    resourcemanager_aidl_interface-ndk

# Kaos Prebuilt Binaries
# busybox_kaos: static Linux binary from prebuilts/arm64/ — provides xzcat, nsenter, etc.
# curl, iw, sqlite3: already installed by AOSP's external/curl, external/iw,
#   external/sqlite modules — do NOT redeclare them here.
PRODUCT_PACKAGES += \
    busybox_kaos \
    tcpdump

# Kaos Init RC → /system/etc/init/ (runs as u:r:init:s0)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/init.kaos.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/init.kaos.rc

# Null composer HAL (kaos/hal/hwcomposer-null/). Disabled 2026-08-01 in favor
# of masking surfaceflinger entirely for SailfishOS/Ubuntu Touch (matching
# real upstream Halium's own approach, kaos/REAL_HALIUM_VOLD_FBE_FINDINGS.md
# Evidence E) -- re-enabled 2026-08-21 for Droidian specifically, whose phoc
# compositor does a real hwservicemanager lookup for
# android.hardware.graphics.composer@2.3::IComposer/default and fatal-aborts
# ("failed to get hwcomposer service") when nothing answers it, unlike
# surfaceflinger/lipstick/lomiri which don't need the composer HAL alive at
# all. SetupManager.java's install_hwcomposer_null_patch() bind-mounts this
# RC over the real vendor composer's RC for Droidian only (gated on the same
# /usr/lib/halium-wrappers/android-service.sh presence check used to select
# the masking path for other distros); SailfishOS/UT continue getting the
# masking path via install_hwcomposer_disabled_patch(), unchanged. The
# binary installs to /system/bin/hw/ on the shared image regardless -- it is
# never started on any distro's boot unless bind-mounted in like this, since
# its own RC is deliberately not installed to /system/etc/init/.
#
# This module is gated on DROIDIAN_BUILD because its HIDL template/resource
# link chain currently fails to resolve under the lineage_perseus systemimage
# link (undefined ComposerResources/ComposerHandleImporter symbols). It can
# still be built explicitly for Droidian work (see DROIDIAN_DEPLOY.md).
ifeq ($(DROIDIAN_BUILD),1)
PRODUCT_PACKAGES += \
    android.hardware.graphics.composer@2.3-service-null
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/hal/hwcomposer-null/android.hardware.graphics.composer@2.3-service-null.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/kaos/hwcomposer-null.rc
endif

# Exclude kaos-touch uinput device from Android InputManager (prevents feedback loop)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/etc/excluded-input-devices.xml:$(TARGET_COPY_OUT_VENDOR)/etc/excluded-input-devices.xml

# Kaos Apps (PhoshApp handles display, legacy Kali app removed)
#
# privapp-permissions-kaos.xml carries the CORRECT, real installed package
# names for these three apps' actual signature|privileged permission
# requests (com.kaos.phosh, com.offsec.nhterm, com.android.aliceagent --
# confirmed live via a system_server dropbox crash: PackageManagerService's
# systemReady() throws an uncaught, fatal IllegalStateException when any
# priv-app requests such a permission that isn't allowlisted anywhere, which
# kills system_server -> zygote's own "exit because system server died"
# response -> an endless restart-storm, since the same apps reinstall
# identically every boot). privapp-permissions-kaos-term.xml/-kaos-app.xml
# below cover stale/different package names (com.kaos.term, com.kaos) that
# don't match any currently-built app and were never fixed after these three
# apps got their current package identities -- kept here since removing them
# isn't needed to fix the crash, but privapp-permissions-kaos.xml is the one
# that actually matters now.
PRODUCT_PACKAGES += \
    KaosTerm \
    PhoshApp \
    AliceAgent \
    privapp-permissions-kaos-term.xml \
    privapp-permissions-kaos-app.xml \
    privapp-permissions-kaos.xml
