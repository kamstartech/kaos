#!/bin/sh
# Build kaos-libseat-fake.so inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
set -e
cd /root
gcc -O2 -Wall -fPIC -shared -o /usr/local/lib/kaos-libseat-fake.so \
    kaos-libseat-fake.c \
    $(pkg-config --cflags --libs libseat) -ldl -lpthread
chmod 755 /usr/local/lib/kaos-libseat-fake.so
