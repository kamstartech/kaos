#!/bin/bash
# Single entry point for building any Kaos-supported distro. The actual
# build mechanics are completely different per OS family (debootstrap
# rootfs vs. an RPM/mb2/mic image via the Sailfish Platform SDK), so this
# never reimplements either -- it only picks which existing script to run
# and forwards whatever args come after the distro name, verbatim, to it.
#
# Usage:
#   ./build-distro.sh ubuntu|debian|kali [--bare] [--no-ui] [--no-net]
#       -> hybris/kaos-configs/build-rootfs.sh --distro <distro> [flags...]
#
#   ./build-distro.sh sailfish build|shell|rpm [flags...]
#       -> hybris/hadk-sdk/build-sailfish-sdk.sh [args...]
#
#   ./build-distro.sh   (no args: interactive picker, full/default build)
set -e

HADK_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS_BUILDER="$HADK_ROOT/hybris/kaos-configs/build-rootfs.sh"
SAILFISH_BUILDER="$HADK_ROOT/hybris/hadk-sdk/build-sailfish-sdk.sh"

usage() {
    cat >&2 <<EOF
Usage: $0 <distro> [args...]

  ubuntu|debian|kali   -> build-rootfs.sh --distro <distro> [args...]
                          (args: --bare / --no-ui / --no-net)
  sailfish              -> build-sailfish-sdk.sh <args...>
                          (args: build | shell | rpm [build_packages.sh flags])

  $0   (no args)        -> interactive picker, full/default build for
                           whichever OS you pick
EOF
    exit 1
}

DISTRO="${1:-}"

if [ -z "$DISTRO" ]; then
    echo "Kaos build-distro -- interactive"
    echo
    echo "  1) ubuntu"
    echo "  2) debian"
    echo "  3) kali"
    echo "  4) sailfish"
    read -r -p "Pick [1-4]: " _pick
    case "$_pick" in
        1) DISTRO="ubuntu" ;;
        2) DISTRO="debian" ;;
        3) DISTRO="kali" ;;
        4) DISTRO="sailfish" ;;
        *) echo "invalid pick '$_pick'" >&2; exit 1 ;;
    esac
    shift $#
else
    shift
fi

case "$DISTRO" in
    ubuntu|debian|kali)
        exec "$ROOTFS_BUILDER" --distro "$DISTRO" "$@"
        ;;
    sailfish)
        if [ $# -eq 0 ]; then
            # No sub-action given (only reachable from the interactive
            # picker, which shift'd away any trailing args) -- "rpm" with
            # no flags is build_packages.sh's own "build everything"
            # default, matching the debian-family full-build default above.
            exec "$SAILFISH_BUILDER" rpm
        fi
        exec "$SAILFISH_BUILDER" "$@"
        ;;
    *)
        echo "Unknown distro: $DISTRO" >&2
        usage
        ;;
esac
