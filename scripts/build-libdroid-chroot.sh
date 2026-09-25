#!/bin/sh
# Build and install libdroid inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
#
# libdroid (github.com/droidian/libdroid, vendored at hybris/mw/libdroid,
# noble branch) is a small AIDL/HIDL-based helper library for Android HAL
# access (leds/vibrator via binder). It's what wlroots' hwcomposer backend
# links against for libdroid-0 -- see build-wlroots-hwcomposer-chroot.sh.
# Its own build-deps are all plain apt packages (installed by build-rootfs.sh
# before this script runs); it needs nothing from android-headers/libhardware.
set -e
# Android 15 headers use Clang nullability annotations (_Nullable/_Nonnull) and
# __INTRODUCED_IN that GCC does not understand. Force-include a shim that
# defines them away before any Android header is parsed.
export CFLAGS="${CFLAGS:+$CFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"
cd /usr/src/libdroid
meson setup build --prefix=/usr --buildtype=release
ninja -C build
ninja -C build install
ldconfig
