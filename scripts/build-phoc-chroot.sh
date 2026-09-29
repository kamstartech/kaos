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
#
# 2026-09-28: this script used to also run `apt-get install -y wget git`
# and `apt-get build-dep -y phoc` itself, right before the git clone
# below. That silently reinstalled stock libwlroots-dev *after*
# wlroots-hwcomposer was already in place from the previous build step,
# clobbering its headers/lib right before phoc-droidian's own build --
# so it compiled against reverted stock wlroots 0.17.1 instead of the
# fork, failing with "too many arguments to wlr_output_layout_create"
# and "no member named 'new_toplevel'" (both real API/struct differences
# the fork's 6414 extra commits introduced). Fixed by moving both apt
# calls into build-rootfs.sh, before wlroots-hwcomposer's own install --
# see the comment there.
set -e

export PKG_CONFIG_PATH="/usr/lib/hybris/pkgconfig:/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# wget/git and phoc's own build-deps (including stock libwlroots-dev) are
# installed by build-rootfs.sh itself now, BEFORE wlroots-hwcomposer's
# build overwrites stock wlroots on disk -- not here. Running `apt-get
# build-dep -y phoc` from this script used to reinstall stock
# libwlroots-dev *after* wlroots-hwcomposer was already in place,
# clobbering the fork's headers/lib right before the meson/ninja build
# below, which then silently compiled phoc-droidian against the wrong
# wlroots API. See build-rootfs.sh's own comment where that apt call now
# lives for the full story.

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
# ours instead of stock. Its build-deps (gtk3/gnome-desktop/etc, the same
# runtime deps phosh-core already needed, plus stock libwlroots-dev) were
# already installed by build-rootfs.sh before wlroots-hwcomposer's own
# build overwrote it on disk -- see this file's top comment. We only use
# those OTHER build-deps here and build against our own already-installed
# wlroots-hwcomposer via PKG_CONFIG_PATH.

git clone --depth 1 --branch group/next/phosh-0.47 \
    https://github.com/droidian/phoc.git /usr/src/phoc-droidian
cd /usr/src/phoc-droidian
# embed-wlroots defaults to 'auto' (subprojects/wlroots.wrap, pinned to
# mainline wlroots 0.18.2 -- NOT our fork, no hwcomposer backend at all).
# With network access meson successfully fetches and builds that
# subproject instead of using our own already-installed
# wlroots-hwcomposer, then fails anyway: meson.build:81's
# cc.has_header(dependencies: wlroots) requires an *external* dependency
# object, which the embedded-subproject path doesn't provide ("Dependency
# must be an external dependency"). Confirmed live 2026-09-25 on the Mac
# build. Force the system/pkg-config path (PKG_CONFIG_PATH above already
# points at our own wlroots-hwcomposer's .pc file) so this can never
# silently link the wrong wlroots even when the subproject fetch would
# have succeeded.
meson setup build --prefix=/usr --buildtype=release -Dxwayland=disabled -Dembed-wlroots=disabled
ninja -C build
ninja -C build install
cd /usr/src
rm -rf phoc-droidian
