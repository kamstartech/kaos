#!/bin/sh
# Build kaos-ui-capture inside a running Kaos chroot namespace.
#
# kaos-ui-capture is a glibc binary that links against the chroot's
# libwayland-client (not bionic), so it cannot be built by AOSP's mka.
# Run this on the Android host while the target distro (e.g. ubuntu)
# is running and reachable via SSH/adb.
#
# Usage:
#   ./build-kaos-ui-capture.sh [distro-id]
#
# The resulting binary is copied to /usr/local/bin/kaos-ui-capture inside
# the chroot and should be picked up from there by build-rootfs.sh when
# the rootfs tarball is regenerated.

DISTRO_ID="${1:-ubuntu}"
CHROOT="/data/.stowaway/$DISTRO_ID"
SRC="$(dirname "$0")/kaos-ui-capture.cpp"
PROTO="$(dirname "$0")/wlr-screencopy-unstable-v1.xml"

if [ ! -d "$CHROOT" ]; then
    echo "Distro rootfs not found: $CHROOT" >&2
    exit 1
fi

if [ ! -f "$SRC" ]; then
    echo "Source not found: $SRC" >&2
    exit 1
fi

if [ ! -f "$PROTO" ]; then
    echo "Protocol XML not found: $PROTO" >&2
    echo "Download it from:" >&2
    echo "  https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-screencopy-unstable-v1.xml" >&2
    exit 1
fi

WORKDIR="$CHROOT/root/uicapture"
adb shell "mkdir -p $WORKDIR"
adb push "$SRC" "$WORKDIR/kaos-ui-capture.cpp"
adb push "$PROTO" "$WORKDIR/wlr-screencopy-unstable-v1.xml"

adb shell "chroot $CHROOT /bin/sh -c '
    cd /root/uicapture &&
    wayland-scanner client-header wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-client-protocol.h &&
    wayland-scanner private-code wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-client-protocol.c &&
    g++ -O2 -Wall -o kaos-ui-capture kaos-ui-capture.cpp \\
        \$(pkg-config --cflags --libs wayland-client) &&
    cp kaos-ui-capture /usr/local/bin/kaos-ui-capture &&
    chmod 755 /usr/local/bin/kaos-ui-capture
'"

echo "kaos-ui-capture built and installed to $CHROOT/usr/local/bin/kaos-ui-capture"
