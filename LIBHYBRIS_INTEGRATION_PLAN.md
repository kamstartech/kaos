# libhybris Integration Plan for Perseus Kaos ROM

## 🎯 Goal
Build libhybris compatibility layers **directly into the Android ROM** to enable:
- Running glibc-based Linux distributions on Android hardware
- Accessing Android hardware drivers from Linux
- Dual-boot or live-switching between Android and Linux
- Full hardware acceleration in Linux mode

## 📦 What We Have

### libhybris Repository Cloned ✅
```
external/libhybris/
├── compat/           # Android compatibility layers
│   ├── camera/      # Camera HAL wrapper
│   ├── media/       # Media HAL wrapper
│   ├── ui/          # UI HAL wrapper
│   ├── input/       # Input HAL wrapper
│   ├── hwc2/        # Hardware Composer v2
│   └── surface_flinger/  # SurfaceFlinger wrapper
├── hybris/          # Core libhybris library
└── utils/           # Utilities
```

### Android Build Files Found ✅
- `compat/*/Android.mk` - Build definitions for compatibility layers
- Supports Android 8+ (our Android 15 is perfect!)
- Multilib support (32/64-bit)

## 🔨 Integration Steps

### Phase 1: Build Compatibility Layers (Current)

#### 1.1 Create Main Android.mk
**File**: `external/libhybris/Android.mk`
```makefile
LOCAL_PATH := $(call my-dir)

# Build compatibility layers
include $(call all-named-subdir-makefiles, compat)
```

#### 1.2 Build Individual Components
Components to build:
- ✅ `libcamera_compat_layer` - Camera access from Linux
- ✅ `libmedia_compat_layer` - Audio/video codecs
- ✅ `libui_compat_layer` - UI/Graphics
- ✅ `libis_compat_layer` - Input system
- ✅ `libhwc2_compat_layer` - Hardware composer
- ✅ `libsf_compat_layer` - SurfaceFlinger

### Phase 2: Install to System

#### 2.1 Add to Device Build
**File**: `kaos/hybris.mk`
```makefile
# libhybris compatibility layers
PRODUCT_PACKAGES += \
    libcamera_compat_layer \
    libmedia_compat_layer \
    libui_compat_layer \
    libis_compat_layer \
    libhwc2_compat_layer \
    libsf_compat_layer

# Install to /vendor for hardware-specific libraries
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*.so,external/libhybris/compat,$(TARGET_COPY_OUT_VENDOR)/lib/libhybris)
```

#### 2.2 Update kaos.mk
**File**: `kaos/kaos.mk`
```makefile
# Include libhybris
$(call inherit-product, kaos/hybris.mk)
```

### Phase 3: Create Linux-side Bridge

#### 3.1 Build glibc Wrapper Libraries
These would be built separately in a Linux chroot:
```bash
# In Kaos chroot:
git clone https://github.com/libhybris/libhybris.git
cd libhybris/hybris
./autogen.sh
./configure --enable-wayland --enable-arch=arm64
make
make install
```

#### 3.2 Symlink Android Libraries
Make Android libs accessible from Linux:
```bash
# Mount Android system libs
mkdir -p /android/system/lib64
mount --bind /system/lib64 /android/system/lib64

# Configure libhybris paths
export HYBRIS_LD_LIBRARY_PATH=/android/system/lib64:/android/vendor/lib64
```

### Phase 4: Create Switching Mechanism

#### 4.1 Mode Scripts
**File**: `/vendor/bin/hybris-mode`
```bash
#!/system/bin/sh
# Hybris mode switcher

case "$1" in
    android)
        # Pure Android mode (default)
        setprop hybris.mode android
        stop linux-container
        ;;
    
    linux)
        # Pure Linux mode
        setprop hybris.mode linux
        start linux-container
        # Stop Android UI
        stop surfaceflinger
        stop zygote
        ;;
    
    hybrid)
        # Both running simultaneously
        setprop hybris.mode hybrid
        start linux-container
        # Keep Android running
        ;;
    
    status)
        getprop hybris.mode
        ;;
esac
```

#### 4.2 Linux Container Service
**File**: `/vendor/etc/init/linux-container.rc`
```
service linux-container /vendor/bin/start-linux.sh
    class late_start
    user root
    group root
    disabled
    oneshot

on property:hybris.mode=linux
    start linux-container

on property:hybris.mode=hybrid
    start linux-container
```

## 🎨 Use Cases

### Use Case 1: Kaos with Full Graphics
```bash
# Switch to Linux mode
hybris-mode linux

# In Linux, access Android GPU
Xwayland -hybris :0 &
export DISPLAY=:0

# Run full GUI Linux apps with hardware acceleration!
firefox
kali-linux-gui
```

### Use Case 2: Dual Environment
```bash
# Hybrid mode - both running
hybris-mode hybrid

# Android apps on one screen
# Linux apps on another screen
# Share clipboard, files, etc.
```

### Use Case 3: Ubuntu Touch / SailfishOS
```bash
# Full mobile Linux OS using our drivers
systemctl start lxc@ubuntu-touch
systemctl start lipstick  # SailfishOS UI
```

## 🔧 Technical Details

### How libhybris Works

```
┌─────────────────────────────────────────┐
│        Linux Process (glibc)             │
│  ┌────────────────────────────────────┐ │
│  │   Your Linux Application           │ │
│  │   (Firefox, Kali tools, etc.)      │ │
│  └──────────────┬─────────────────────┘ │
│                 │                        │
│                 ▼                        │
│  ┌────────────────────────────────────┐ │
│  │    libhybris (glibc wrapper)       │ │
│  │  • Intercepts Android API calls    │ │
│  │  • Translates glibc ↔ bionic       │ │
│  └──────────────┬─────────────────────┘ │
└─────────────────┼─────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────┐
│     Android System (bionic)              │
│  ┌────────────────────────────────────┐ │
│  │  libcamera_compat_layer.so         │ │
│  │  libmedia_compat_layer.so          │ │
│  │  libui_compat_layer.so             │ │
│  └──────────────┬─────────────────────┘ │
│                 │                        │
│                 ▼                        │
│  ┌────────────────────────────────────┐ │
│  │   Android HALs (Hardware drivers)  │ │
│  │  • Camera HAL                      │ │
│  │  • Graphics HAL                    │ │
│  │  • Audio HAL                       │ │
│  └──────────────┬─────────────────────┘ │
│                 │                        │
└─────────────────┼─────────────────────────┘
                  │
                  ▼
         ┌────────────────┐
         │   HARDWARE     │
         │  (Perseus Mi8) │
         └────────────────┘
```

### What Gets Built

**Android Side (bionic libraries)**:
```
/vendor/lib64/libhybris/
├── libcamera_compat_layer.so     # Camera access
├── libmedia_compat_layer.so      # Codecs/audio
├── libui_compat_layer.so         # Graphics
├── libis_compat_layer.so         # Input
├── libhwc2_compat_layer.so       # Compositor
└── libsf_compat_layer.so         # SurfaceFlinger
```

**Linux Side (glibc wrappers)** - built in chroot:
```
/usr/lib/aarch64-linux-gnu/libhybris/
├── libEGL.so.1      → wraps Android EGL
├── libGLESv2.so.2   → wraps Android GLES
├── libhardware.so   → wraps Android hardware
├── libcamera.so     → wraps Android camera
└── libmedia.so      → wraps Android media
```

## 📋 Build Steps (Actual Commands)

### Step 1: Create Android.mk Files

```bash
# Create main build file
cat > external/libhybris/Android.mk << 'EOF'
LOCAL_PATH := $(call my-dir)
include $(call all-named-subdir-makefiles, compat)
EOF

# Create common build configuration
cat > external/libhybris/compat/Android.common.mk << 'EOF'
# Common configuration for libhybris compat layers
HYBRIS_PATH := $(LOCAL_PATH)/../hybris

LOCAL_CFLAGS += \
    -DANDROID_VERSION_MAJOR=$(PLATFORM_SDK_VERSION) \
    -Wno-unused-parameter \
    -Wno-unused-variable

LOCAL_C_INCLUDES += \
    $(HYBRIS_PATH)/include
EOF
```

### Step 2: Add to Device Build

```bash
# Create hybris.mk
cat > kaos/hybris.mk << 'EOF'
# libhybris compatibility layers for Linux compatibility

PRODUCT_PACKAGES += \
    libcamera_compat_layer \
    libmedia_compat_layer \
    libui_compat_layer \
    libis_compat_layer \
    libhwc2_compat_layer \
    libsf_compat_layer

# Properties
PRODUCT_PROPERTY_OVERRIDES += \
    hybris.enabled=true \
    hybris.mode=android
EOF

# Include in kaos.mk
echo "" >> kaos/kaos.mk
echo "# libhybris support" >> kaos/kaos.mk
echo '$(call inherit-product, kaos/hybris.mk)' >> kaos/kaos.mk
```

### Step 3: Build!

```bash
# Build libhybris modules
make -j$(nproc) \
    libcamera_compat_layer \
    libmedia_compat_layer \
    libui_compat_layer \
    libis_compat_layer \
    libhwc2_compat_layer \
    libsf_compat_layer
```

### Step 4: Test

```bash
# Check if built
find out/target/product/perseus -name "*compat_layer.so"

# Should see:
# out/target/product/perseus/vendor/lib64/libcamera_compat_layer.so
# out/target/product/perseus/vendor/lib64/libmedia_compat_layer.so
# etc.
```

## 🌟 Benefits

### For Kaos:
- ✅ **Full GPU acceleration** in Kali Linux GUI
- ✅ **Native Linux apps** with hardware support
- ✅ **Better performance** than VNC/chroot
- ✅ **Direct hardware access** from Linux

### For General Use:
- ✅ **Ubuntu Touch** as an option
- ✅ **SailfishOS** compatibility
- ✅ **Droidian** support
- ✅ **PostmarketOS** potential

### For Development:
- ✅ **Test Linux apps** on real hardware
- ✅ **Cross-platform development**
- ✅ **Driver debugging** from Linux
- ✅ **Open source graphics stack**

## 🚀 Future Enhancements

### Phase 5: Wayland Support
- Build Qt5 QPA Hwcomposer plugin
- Run Wayland compositor on Android HWC
- Full desktop Linux experience

### Phase 6: Container Integration
- LXC containers for different Linux distros
- systemd-nspawn for lightweight containers
- Docker with GPU support

### Phase 7: Advanced Features
- Hot-switching between Android/Linux
- Shared clipboard/notifications
- Convergence mode (phone → desktop)
- Multi-window Linux apps in Android

## 📚 References

- **libhybris**: https://github.com/libhybris/libhybris
- **Halium Project**: https://halium.org/
- **Ubuntu Touch HAL**: https://github.com/ubports/
- **SailfishOS HADK**: https://docs.sailfishos.org/Develop/HADK/
- **Droidian**: https://github.com/droidian/

## ✅ Next Steps

1. **Create Android.mk files** ← Start here
2. **Build compat layers**
3. **Test in current ROM**
4. **Build glibc side in chroot**
5. **Create switching scripts**
6. **Package as flashable module**

**Status**: Ready to implement! Let's start with Step 1.
