#!/bin/sh
# Build kaos-selinux-disable.so inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
set -e
cd /root
gcc -O2 -Wall -fPIC -shared -o /usr/local/lib/kaos-selinux-disable.so \
    kaos-selinux-disable.c
chmod 755 /usr/local/lib/kaos-selinux-disable.so
