LOCAL_PATH := $(call my-dir)

# busybox_kaos — static ARM64 Linux multicall binary.
#
# This is deliberately a static GNU/Linux binary (not an Android binary):
#   - Provides xzcat/xz, tar, nsenter, and other tools missing from toybox.
#   - Works on the Android host side (static Linux binaries run fine on Linux kernel)
#     and inside the Linux chroot namespace.
#   - Binary lives in prebuilts/arm64/busybox_kaos — rescued from a prior build;
#     rebuild from https://github.com/offensive-security/nethunter-utils if needed.
#
# All other tools (curl, iw, sqlite3) are already provided by AOSP's external/
# modules as dynamic Android binaries and must NOT be redefined here.
include $(CLEAR_VARS)
LOCAL_MODULE            := busybox_kaos
LOCAL_MODULE_CLASS      := EXECUTABLES
LOCAL_SRC_FILES         := arm64/busybox_kaos
LOCAL_MODULE_PATH       := $(TARGET_OUT)/bin
LOCAL_CHECK_ELF_FILES   := false
LOCAL_STRIP_MODULE      := false
include $(BUILD_PREBUILT)
