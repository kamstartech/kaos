#!/bin/sh
# Build kaos-ui-capture inside a Kaos rootfs chroot.
# Called by hybris/kaos-configs/build-rootfs.sh during rootfs creation.
set -e
cd /root
wayland-scanner client-header wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-client-protocol.h
wayland-scanner private-code wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1-client-protocol.c
g++ -O2 -Wall -o /usr/local/bin/kaos-ui-capture kaos-ui-capture.cpp \
    $(pkg-config --cflags --libs wayland-client)
chmod 755 /usr/local/bin/kaos-ui-capture
