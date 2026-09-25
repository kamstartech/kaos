#!/bin/sh
# Build and install libhybris's gralloc/egl-hwcomposer/glesv/hwc2 components
# inside a Kaos rootfs chroot. Called by hybris/kaos-configs/build-rootfs.sh
# during rootfs creation.
#
# wlroots-hwcomposer links against libgralloc, hwcomposer_window (the
# EGL_PLATFORM=hwcomposer backend), and libhwc2 -- all libhybris components
# (hybris/mw/libhybris, already proven working against this exact device's
# Android 15 vendor HAL by the SailfishOS build: same source, same
# android-headers). This is a *fresh* build for Ubuntu/glibc though -- the
# .so/.la artifacts already sitting in that source tree are from SailfishOS's
# own in-tree mb2/RPM build (different target, different libc build config),
# so this uses an out-of-tree build directory to avoid any contamination
# from that stale state, and targets only the subdirs we actually need
# rather than the project's full default SUBDIRS (which also includes
# camera/media/wifi/vulkan/nfc -- more headers than extract-headers.sh
# extracted, and not needed for the hwcomposer backend).
#
# --libdir=/usr/lib/hybris (not plain /usr/lib, not the multiarch dir):
# libhybris also builds its own libEGL.so/libGLESv2.so, which collide by
# SONAME with the apt-installed Mesa ones (libegl1-mesa/libgles2-mesa,
# installed for the DRM backend's own use). Confirmed live 2026-09-24: with
# both providers on the default search path, Mesa's libEGL won -- it has no
# idea what EGL_PLATFORM=hwcomposer means, so it fell back to trying X11
# ("xcb_connect failed"). Isolating libhybris's whole stack into its own
# directory (the same HYBRIS_LD_LIBRARY_PATH idiom real Halium/SailfishOS
# builds use) and having phoc's environment prefix LD_LIBRARY_PATH with it
# (kaos.init) is what actually decides which one wins, instead of leaving it
# to default search-order luck.
#
# Order matters within the list below (gralloc before egl/hwc2, which link
# against it via $(top_builddir) build-tree references, not an install
# step in between; glesv1/glesv2 only need common, already built first).
set -e

# The vendored source tree still carries SailfishOS's own in-tree mb2/RPM
# build state (Makefiles, config.status, .libs/*.o from a completely
# different target). `configure` refuses to run out-of-tree against an
# already-configured source dir ("run make distclean there first") -- but
# `make distclean` itself invokes the *original* build's toolchain
# assumptions, which don't hold here, so strip the known autotools-generated
# artifacts directly instead of trusting it to work standalone.
find /usr/src/libhybris -type f \( \
    -name Makefile -o -name config.status -o -name config.log \
    -o -name config.h -o -name '*.lo' -o -name '*.la' -o -name '*.o' \
    \) -delete
find /usr/src/libhybris -type d -name '.libs' -o -type d -name '.deps' \
    | xargs -r rm -rf

# build-rootfs.sh copies this whole source tree into the container with a
# plain `cp -r` (not `cp -a`), which does NOT preserve mtimes -- every file
# gets "now" as its mtime, in filesystem-traversal order. That makes the
# relative ordering between configure.ac/Makefile.am (sources) and their
# already-generated aclocal.m4/configure/Makefile.in effectively random
# after every single build, not just for this distro. When configure.ac
# ends up looking newer than aclocal.m4 by copy-order luck, automake's own
# maintainer-mode rules in the generated Makefile try to regenerate it via
# the *exact* aclocal version that originally produced it (aclocal-1.16),
# which this build container doesn't have -- confirmed live 2026-09-25
# building kali ("aclocal-1.16: command not found", Makefile:421). Force
# the correct relative order back explicitly, in dependency order, rather
# than depending on autotools regenerating anything here.
( cd /usr/src/libhybris/hybris \
  && touch configure.ac Makefile.am \
  && touch aclocal.m4 \
  && touch configure config.h.in \
  && touch Makefile.in )

# Android 15 headers use Clang nullability annotations (_Nullable/_Nonnull) and
# __INTRODUCED_IN that GCC does not understand. Force-include a shim that
# defines them away before any Android header is parsed.
export CFLAGS="${CFLAGS:+$CFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }-include /usr/src/android-headers/kaos-gcc-compat.h"

BUILD_DIR=/usr/src/libhybris-build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
/usr/src/libhybris/hybris/configure \
    --prefix=/usr \
    --libdir=/usr/lib/hybris \
    --enable-arch=arm64 \
    --with-android-headers=/usr/src/android-headers

# Deliberately NOT registered via /etc/ld.so.conf.d/ or ldconfig -- that
# would put /usr/lib/hybris back on the *default* system search path for
# every process, recreating the exact Mesa-vs-libhybris collision this
# isolates against. Only phoc's own LD_LIBRARY_PATH (set in kaos.init) is
# meant to see this directory; ldconfig here is only for the libs installed
# to /usr elsewhere in this build (gralloc/hwc2/etc. from earlier scripts),
# unaffected by this directory's contents.
for d in include common properties hardware ui gralloc libsync platforms egl glesv1 glesv2 hwc2; do
    make -j"$(nproc)" -C "$d"
    make -C "$d" install
done
ldconfig
