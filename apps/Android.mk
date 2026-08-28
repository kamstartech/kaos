LOCAL_PATH := $(call my-dir)

# Prebuilt KaosTerm removed - see kaos-term/Android.bp

# Install privapp permissions for Terminal
include $(CLEAR_VARS)
LOCAL_MODULE := privapp-permissions-kaos-term.xml
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT_SYSTEM_EXT)/etc/permissions
LOCAL_SRC_FILES := $(LOCAL_MODULE)
include $(BUILD_PREBUILT)

# Install privapp permissions for Kaos App
include $(CLEAR_VARS)
LOCAL_MODULE := privapp-permissions-kaos-app.xml
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT_SYSTEM_EXT)/etc/permissions
LOCAL_SRC_FILES := $(LOCAL_MODULE)
include $(BUILD_PREBUILT)

# Include Kaos App build (built from source)
# The app's own Android.mk will handle the Gradle build

