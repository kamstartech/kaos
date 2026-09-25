#!/bin/sh
# Build and install a phoc compatible with our wlroots-hwcomposer fork,
# inside a Kaos rootfs chroot. Called by hybris/kaos-configs/build-rootfs.sh
# during rootfs creation, after build-wlroots-hwcomposer-chroot.sh.
#
# Why this exists: Ubuntu's own phoc package (0.38.0+ds-1) is compiled
# against stock wlroots 0.17.1's API/struct layout. wlroots-hwcomposer
# (github.com/kamstartech/wlroots) is 0.17.4 plus 6414 commits tracking
# Droidian's own ongoing wlroots development -- genuinely API-incompatible
# with stock 0.17.1, not just a soname/packaging difference (confirmed live
# 2026-09-25: stock phoc linked against our fork SIGSEGVs deterministically,
# ~1s after "Enabling shell mode", every single boot, in
# wl_list_insert()/wl_signal_emit_mutable() -- the crash was landing on a
# wlroots-internal sentinel object instead of a real wl_list, because
# wlr_output_layout_create() gained a required struct wl_display* argument
# somewhere in those 6414 commits and stock phoc's compiled offsets never
# accounted for it). See kaos/SESSION_HANDOFF entries around 2026-09-25 for
# the full crash analysis.
#
# The fix is Droidian's own phoc fork (github.com/droidian/phoc,
# group/next/phosh-0.47 branch, Phosh 0.47.0) -- built specifically to
# track a wlroots fork like ours, not Ubuntu's stock one. Confirmed live:
# builds and links cleanly against wlroots-hwcomposer's actual headers/lib,
# and the resulting phoc survives past "Enabling shell mode" indefinitely
# (was 100% deterministic SIGSEGV before, tested well past 50s after).
#
# Two build-environment gaps this also works around (fix once, here, not
# per-boot):
#   1. Ubuntu Noble ships pixman 0.42.2; phosh 0.47.0's phoc wants >= 0.43.4.
#      No functional pixman API dependency found -- just build pixman
#      0.46.4 from source and let it overwrite the older shared library
#      (same soname, so ABI-compatible for anything else linking it).
#   2. Xwayland disabled (-Dxwayland=disabled): phoc 0.47.0's
#      xwayland-surface.c uses struct wlr_xwayland_surface's opacity/
#      set_opacity fields, which our wlroots fork doesn't have (Droidian's
#      own wlroots fork apparently added them separately; ours hasn't
#      picked that up). Not worth chasing for a mobile Wayland-only shell --
#      X11 app support is a real, known, accepted loss here, not a mistake.
set -e

export PKG_CONFIG_PATH="/usr/lib/hybris/pkgconfig:/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# Fresh debootstrap images don't have wget or git by default (confirmed
# live 2026-09-25 building kali: "wget: not found") -- the live device this
# was first tested on already had wget installed from earlier ad-hoc work,
# masking this gap. Both are needed below (pixman tarball, phoc git clone).
apt-get install -y wget git

# Gap 1: newer pixman. dpkg will still think libpixman-1-0 owns the old
# .so.0 file underneath this -- same acceptable tradeoff wlroots-hwcomposer's
# own build script already documents for libwlroots.so.12.
cd /usr/src
wget -q https://cairographics.org/releases/pixman-0.46.4.tar.gz
tar xf pixman-0.46.4.tar.gz
rm -f pixman-0.46.4.tar.gz
cd pixman-0.46.4
meson setup build --prefix=/usr --buildtype=release
ninja -C build
ninja -C build install
cd /usr/src
rm -rf pixman-0.46.4

# Gap 2 + the real fix: Droidian's own phoc, tracking a wlroots fork like
# ours instead of stock. apt build-dep pulls in gtk3/gnome-desktop/etc (the
# same runtime deps phosh-core already needed) -- letting it also pull
# libwlroots-dev is harmless here since we never `apt install phoc` again
# after this; we only use its OTHER build-deps and build against our own
# already-installed wlroots-hwcomposer via PKG_CONFIG_PATH regardless of
# whatever libwlroots-dev apt installed alongside it.
apt-get build-dep -y phoc

git clone --depth 1 --branch group/next/phosh-0.47 \
    https://github.com/droidian/phoc.git /usr/src/phoc-droidian
cd /usr/src/phoc-droidian
meson setup build --prefix=/usr --buildtype=release -Dxwayland=disabled
ninja -C build
ninja -C build install
cd /usr/src
rm -rf phoc-droidian
