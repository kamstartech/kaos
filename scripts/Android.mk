LOCAL_PATH := $(call my-dir)

# unshare_kaos — custom unshare(1) compiled from source.
# Required because Android's toybox unshare lacks -C (cgroup namespace) support.
include $(CLEAR_VARS)
LOCAL_MODULE            := unshare_kaos
LOCAL_MODULE_CLASS      := EXECUTABLES
LOCAL_SRC_FILES         := unshare.c
LOCAL_CFLAGS            := -Wall -static
LOCAL_FORCE_STATIC_EXECUTABLE := true
include $(BUILD_EXECUTABLE)

# kaos-display-bridge — SF surface lifecycle daemon for Phosh display.
# Talks to SurfaceFlinger directly via libgui (no libhybris compat layer --
# see header comment in kaos-display-bridge.cpp for why).
include $(CLEAR_VARS)
LOCAL_MODULE            := kaos-display-bridge
LOCAL_MODULE_TAGS       := optional
LOCAL_SRC_FILES         := kaos-display-bridge.cpp
LOCAL_SHARED_LIBRARIES  := libcutils libutils liblog libEGL libGLESv2 libbinder libgui libui
LOCAL_CFLAGS            := -Wall -Werror
include $(BUILD_EXECUTABLE)

# kaos-display-ctl — CLI to send show/hide/quit to kaos-display-bridge.
include $(CLEAR_VARS)
LOCAL_MODULE            := kaos-display-ctl
LOCAL_MODULE_TAGS       := optional
LOCAL_SRC_FILES         := kaos-display-ctl.c
LOCAL_CFLAGS            := -Wall -Werror
include $(BUILD_EXECUTABLE)

# kaos-touch-input — uinput touchscreen bridge for Phosh native touch.
include $(CLEAR_VARS)
LOCAL_MODULE            := kaos-touch-input
LOCAL_MODULE_TAGS       := optional
LOCAL_SRC_FILES         := kaos-input.c
LOCAL_CFLAGS            := -Wall -Werror
include $(BUILD_EXECUTABLE)

# kaos.c — compiled Kaos helper binary.
include $(CLEAR_VARS)
LOCAL_MODULE            := kaos
LOCAL_MODULE_CLASS      := EXECUTABLES
LOCAL_SRC_FILES         := kaos.c
LOCAL_CFLAGS            := -Wall -Werror
include $(BUILD_EXECUTABLE)
