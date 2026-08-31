#!/bin/sh
# Build kaos-libseat-fake.so inside a running Kaos chroot namespace.
#
# The shim is a glibc .so that LD_PRELOADs into phoc/wlroots to provide a
# fake libseat seat in a namespace where logind reports CanGraphical=no.
# It must be built with the chroot's toolchain, not AOSP's bionic toolchain.
#
# Usage:
#   ./build-kaos-libseat-fake.sh [distro-id]

DISTRO_ID="${1:-kaos_mainline}"
CHROOT="/data/.stowaway/$DISTRO_ID"
SRC="$(dirname "$0")/kaos-libseat-fake.c"

if ! adb shell "[ -d '$CHROOT' ]" >/dev/null 2>&1; then
    echo "Distro rootfs not found on device: $CHROOT" >&2
    exit 1
fi

if [ ! -f "$SRC" ]; then
    echo "Source not found: $SRC" >&2
    exit 1
fi

WORKDIR="$CHROOT/root/libseatfake"
adb shell "mkdir -p $WORKDIR"
adb push "$SRC" "$WORKDIR/kaos-libseat-fake.c"

adb shell "chroot $CHROOT /bin/sh -c '
    set -e
    cd /root/libseatfake
    gcc -O2 -Wall -fPIC -shared -o kaos-libseat-fake.so \
        kaos-libseat-fake.c \
        \$(pkg-config --cflags --libs libseat) -ldl -lpthread
    mkdir -p /usr/local/lib
    cp kaos-libseat-fake.so /usr/local/lib/kaos-libseat-fake.so
    chmod 755 /usr/local/lib/kaos-libseat-fake.so
'"

echo "kaos-libseat-fake.so built and installed."
echo "Add to phosh.service Environment: LD_PRELOAD=/usr/local/lib/kaos-libseat-fake.so"
