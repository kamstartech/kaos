#!/bin/sh
# Build and install libgbinder inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
#
# libgbinder (github.com/sailfishos/libgbinder, already vendored at
# hybris/mw/libgbinder for the SailfishOS build) provides the pkg-config
# module 'libgbinder' that libdroid's meson.build depends on. Not packaged
# in Ubuntu noble's archive (confirmed live: `apt-get install libgbinder-dev`
# -> "Unable to locate package"), so it's built from source here, same as
# libglibutil (its own build-dep, built by build-libglibutil-chroot.sh
# immediately before this runs) and libdroid/wlroots-hwcomposer.
set -e
cd /usr/src/libgbinder
make -j"$(nproc)" release pkgconfig
make install-dev
ldconfig
