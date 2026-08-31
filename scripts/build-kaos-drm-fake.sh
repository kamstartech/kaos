#!/bin/sh
# Build kaos-drm-fake.so inside a running Kaos chroot namespace.
#
# The shim is a glibc .so that LD_PRELOADs into phoc/wlroots, so it must be
# built with the chroot's toolchain, not AOSP's bionic toolchain.
#
# Usage:
#   ./build-kaos-drm-fake.sh [distro-id]

DISTRO_ID="${1:-kaos_mainline}"
CHROOT="/data/.stowaway/$DISTRO_ID"
SRC="$(dirname "$0")/kaos-drm-fake.c"

if ! adb shell "[ -d '$CHROOT' ]" >/dev/null 2>&1; then
    echo "Distro rootfs not found on device: $CHROOT" >&2
    exit 1
fi

if [ ! -f "$SRC" ]; then
    echo "Source not found: $SRC" >&2
    exit 1
fi

WORKDIR="$CHROOT/root/drmfake"
adb shell "mkdir -p $WORKDIR"
adb push "$SRC" "$WORKDIR/kaos-drm-fake.c"

adb shell "chroot $CHROOT /bin/sh -c '
    set -e
    cd /root/drmfake
    gcc -O2 -Wall -fPIC -shared -o kaos-drm-fake.so \
        kaos-drm-fake.c \
        $(pkg-config --cflags --libs libdrm libseat) -ldl -lpthread
    cp kaos-drm-fake.so /usr/local/lib/kaos-drm-fake.so
    chmod 755 /usr/local/lib/kaos-drm-fake.so
'"

echo "kaos-drm-fake.so built and installed."
echo "Run phoc with: LD_PRELOAD=/usr/local/lib/kaos-drm-fake.so WLR_BACKENDS=drm,libinput /usr/bin/phoc ..."
