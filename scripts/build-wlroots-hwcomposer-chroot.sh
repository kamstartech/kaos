#!/bin/sh
# Build and install our wlroots hwcomposer-backend fork inside a Kaos rootfs
# chroot. Called by hybris/kaos-configs/build-rootfs.sh during rootfs
# creation.
#
# wlroots-hwcomposer (github.com/kamstartech/wlroots, forked from
# droidian/wlroots, vendored at hybris/mw/wlroots-hwcomposer, noble branch):
# adds a wlr_backend that renders through the real Android HWComposer HIDL
# service via libhybris, instead of DRM/KMS+Mesa/GBM -- the same approach
# SailfishOS's lipstick already uses successfully on this device
# (QT_QPA_PLATFORM=hwcomposer / EGL_PLATFORM=hwcomposer), and what Droidian
# uses for phosh via phosh-config-hwcomposer. Building 'auto' backends (not
# just hwcomposer) keeps the existing DRM/libinput/x11 backends compiled in
# too -- phoc picks the backend at runtime via WLR_BACKENDS=, unchanged here.
#
# Its meson.build gates the hwcomposer backend on two dependency() lookups:
#   android-headers (>=9.0.0)  -- installed from the extracted headers below
#   libdroid-0                 -- installed by build-libdroid-chroot.sh
# Everything else it needs is a plain apt package (installed by
# build-rootfs.sh before this script runs).
#
# NOTE: this ninja-installs straight over whatever libwlroots
# phosh-core/phoc already pulled in via apt as a dependency. dpkg's database
# will still think the stock package owns those files -- acceptable for a
# first working build to verify the hwcomposer path actually renders; if it
# does, this should become a proper .deb (dpkg-buildpackage) so dpkg stays
# consistent, same as any other package here.
#
# SONAME mismatch: upstream wlroots hardcodes soversion='12a' (meson.build),
# so our build installs as libwlroots.so.12a. Ubuntu's libwlroots12t64
# package patches that away to plain libwlroots.so.12 for its own packaging
# -- and phoc is linked against that exact name (its ELF NEEDED entry is
# libwlroots.so.12, not .12a). Without the symlink below, ninja install's
# overwrite is a no-op in practice: phoc's dynamic loader would still
# resolve the untouched stock apt file, not ours. Confirmed live 2026-09-24:
# a completed build had every library in place but this exact gap -- phoc
# would have silently kept loading the old DRM-only libwlroots.so.12.
set -e

# Android 15 headers use Clang nullability annotations (_Nullable/_Nonnull) and
# __INTRODUCED_IN that GCC does not understand. Force-include a shim that
# defines them away before any Android header is parsed.
export CFLAGS="${CFLAGS:+$CFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"

# libhybris (libgralloc, libhwc2, hwcomposer-egl/hybris-hwcomposerwindow)
# installs to /usr/lib/hybris now, not the standard system libdir -- see
# build-libhybris-chroot.sh's own note on why (avoids colliding with apt's
# Mesa EGL/GLESv2 by SONAME). pkg-config and find_library() both need to be
# told where to look, or this dependency resolution regresses to "not
# found" for gralloc/hwc2/etc, which built fine before that isolation.
export PKG_CONFIG_PATH="/usr/lib/hybris/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LDFLAGS="-L/usr/lib/hybris ${LDFLAGS:-}"
export LD_LIBRARY_PATH="/usr/lib/hybris${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd /usr/src/android-headers
make install PREFIX=/usr/local
ldconfig

cd /usr/src/wlroots-hwcomposer
meson setup build --prefix=/usr --buildtype=release -Dbackends=auto -Dexamples=false
ninja -C build
ninja -C build install

# Force phoc's actual NEEDED name (libwlroots.so.12) to resolve to our
# build (libwlroots.so.12a), replacing whatever apt installed there -- see
# the SONAME mismatch note above. Ubuntu/aarch64 multiarch libdir, same
# convention build-mesa-kgsl.sh already hardcodes for this exact target.
ln -sf libwlroots.so.12a /usr/lib/aarch64-linux-gnu/libwlroots.so.12
ldconfig
