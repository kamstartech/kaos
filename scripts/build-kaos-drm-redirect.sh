#!/bin/sh
# Build kaos-drm-redirect.so inside a running Kaos chroot namespace.
#
# The shim is a glibc .so that LD_PRELOADs into phoc/wlroots, so it must be
# built with the chroot's toolchain, not AOSP's bionic toolchain.
#
# Usage:
#   ./build-kaos-drm-redirect.sh [distro-id]

DISTRO_ID="${1:-kaos_mainline}"
CHROOT="/data/.stowaway/$DISTRO_ID"
SRC="$(dirname "$0")/kaos-drm-redirect.c"
TEST_SRC="$(dirname "$0")/kaos-drm-redirect-test.c"

if ! adb shell "[ -d '$CHROOT' ]" >/dev/null 2>&1; then
    echo "Distro rootfs not found on device: $CHROOT" >&2
    exit 1
fi

if [ ! -f "$SRC" ]; then
    echo "Source not found: $SRC" >&2
    exit 1
fi

WORKDIR="$CHROOT/root/drmredirect"
adb shell "mkdir -p $WORKDIR"
adb push "$SRC" "$WORKDIR/kaos-drm-redirect.c"
adb push "$TEST_SRC" "$WORKDIR/kaos-drm-redirect-test.c"

adb shell "chroot $CHROOT /bin/sh -c '
    cd /root/drmredirect &&
    gcc -O2 -Wall -fPIC -shared -o kaos-drm-redirect.so \
        kaos-drm-redirect.c \\
        \$(pkg-config --cflags --libs libdrm) -ldl &&
    gcc -O2 -Wall -o kaos-drm-redirect-test \
        kaos-drm-redirect-test.c \\
        \$(pkg-config --cflags --libs libdrm) &&
    cp kaos-drm-redirect.so /usr/local/lib/kaos-drm-redirect.so &&
    cp kaos-drm-redirect-test /usr/local/bin/kaos-drm-redirect-test &&
    chmod 755 /usr/local/lib/kaos-drm-redirect.so &&
    chmod 755 /usr/local/bin/kaos-drm-redirect-test
'"

echo "kaos-drm-redirect.so built and installed."
echo "Test without shim:  kaos-drm-redirect-test /dev/dri/card0"
echo "Test with shim:     LD_PRELOAD=/usr/local/lib/kaos-drm-redirect.so kaos-drm-redirect-test /dev/dri/card0"
