#!/bin/sh
# Build and install libglibutil inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
#
# libglibutil (github.com/sailfishos/libglibutil, vendored at
# hybris/mw/libglibutil) is libgbinder's own build-dep -- see
# build-libgbinder-chroot.sh. Only needs glib-2.0/gobject-2.0 (plain apt
# packages, installed by build-rootfs.sh before this script runs).
set -e
cd /usr/src/libglibutil
make release pkgconfig
make install-dev
ldconfig
