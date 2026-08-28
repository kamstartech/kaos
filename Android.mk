# Kaos Integration
LOCAL_PATH := $(call my-dir)

# Save path before sub-makefiles overwrite LOCAL_PATH via their own my-dir calls.
_KAOS_PATH := $(LOCAL_PATH)

include $(call all-makefiles-under,$(LOCAL_PATH))

# Guard: verify busybox_kaos prebuilt is present.
# Uses _KAOS_PATH, not LOCAL_PATH, because all-makefiles-under clobbers LOCAL_PATH.
# curl/iw/sqlite3 come from AOSP external/ and do not need checking here.
$(if $(wildcard $(_KAOS_PATH)/prebuilts/arm64/busybox_kaos),,\
    $(error Kaos: prebuilts/arm64/busybox_kaos is missing. \
        Copy the static ARM64 binary from a prior build output or \
        rebuild from https://github.com/offensive-security/nethunter-utils))

