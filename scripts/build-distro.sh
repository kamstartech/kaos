#!/bin/bash
# Single entry point for building any Kaos-supported distro. The actual
# build mechanics are completely different per OS family (debootstrap
# rootfs vs. an RPM/mb2/mic image via the Sailfish Platform SDK), so this
# never reimplements either -- it only picks which existing script to run
# and forwards whatever args come after the distro name, verbatim, to it.
#
# Usage (same --distro flag for every OS, matching build-rootfs.sh's own
# calling convention, even though sailfish forwards to a completely
# different builder underneath):
#   ./build-distro.sh --distro ubuntu|debian|kali [--bare] [--no-ui] [--no-net]
#       -> hybris/kaos-configs/build-rootfs.sh --distro <distro> [flags...]
#
#   ./build-distro.sh --distro sailfish [build|shell|rpm] [flags...]
#       -> hybris/hadk-sdk/build-sailfish-sdk.sh [args...]  (default: rpm)
#
#   ./build-distro.sh   (no args: interactive picker, full/default build)
set -e

HADK_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS_BUILDER="$HADK_ROOT/hybris/kaos-configs/build-rootfs.sh"
SAILFISH_BUILDER="$HADK_ROOT/hybris/hadk-sdk/build-sailfish-sdk.sh"
TIMING_LOG="$(dirname "$0")/.build-times.log"

format_duration() {
    local s=$1
    printf '%dh%02dm%02ds' $((s/3600)) $((s%3600/60)) $((s%60))
}

# Neither build-rootfs.sh nor build-sailfish-sdk.sh tracks how long a build
# takes, so this wraps whichever one runs: real elapsed time to stderr when
# it finishes, plus one line appended to .build-times.log so past runs stay
# reviewable. Can't use exec here (it replaces this process, so there'd be
# nothing left to measure the end time or run the trailing log write) --
# `... || rc=$?` is used instead of a plain `cmd; rc=$?` so a failing build
# doesn't trip `set -e` before its exit code is captured.
run_timed() {
    local label="$1" start end rc=0
    shift
    start=$(date +%s)
    "$@" || rc=$?
    end=$(date +%s)
    local dur
    dur=$(format_duration $((end - start)))
    printf '[build-distro] %s finished in %s (exit %d)\n' "$label" "$dur" "$rc" >&2
    printf '%s %s %s exit=%d\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$label" "$dur" "$rc" >> "$TIMING_LOG"
    return "$rc"
}

usage() {
    cat >&2 <<EOF
Usage: $0 --distro <name> [args...]

  --distro ubuntu|debian|kali  -> build-rootfs.sh --distro <distro> [args...]
                                  (args: --bare / --no-ui / --no-net)
  --distro sailfish             -> build-sailfish-sdk.sh <args...>
                                   (args: build | shell | rpm [build_packages.sh flags])

  $0   (no args)                -> interactive picker, full/default build for
                                   whichever OS you pick
EOF
    exit 1
}

DISTRO=""
INTERACTIVE_EXTRA_ARGS=()

if [ $# -eq 0 ]; then
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
    echo

    # build-rootfs.sh's own interactive picker used to ask these two
    # follow-ups (WITH_UI/WITH_NET) before this script existed -- moved
    # here now that this is the single interactive entry point, forwarded
    # as --no-ui/--no-net since build-rootfs.sh is always invoked
    # non-interactively (with --distro) from this script.
    if [ "$DISTRO" != "sailfish" ]; then
        WITH_UI=1
        WITH_NET=1

        read -r -p "Include GPU/display stack -- Mesa KGSL, phosh-core, libseat-fake, ui-capture? [Y/n]: " _pick
        case "$_pick" in
            [nN]*) WITH_UI=0; INTERACTIVE_EXTRA_ARGS+=(--no-ui) ;;
        esac

        read -r -p "Include networking -- dhcpcd/wpasupplicant, kaos-wifi units, static DNS? [Y/n]: " _pick
        case "$_pick" in
            [nN]*) WITH_NET=0; INTERACTIVE_EXTRA_ARGS+=(--no-net) ;;
        esac
        echo

        echo "About to build:"
        echo "  distro:     $DISTRO"
        echo "  UI/display: $([ "$WITH_UI" = "1" ] && echo yes || echo no)"
        echo "  networking: $([ "$WITH_NET" = "1" ] && echo yes || echo no)"
        read -r -p "Proceed? [Y/n]: " _pick
        case "$_pick" in
            [nN]*) echo "[build-distro] aborted" >&2; exit 1 ;;
        esac
        echo
    fi
elif [ "$1" = "--distro" ]; then
    DISTRO="${2:-}"
    [ -z "$DISTRO" ] && usage
    shift 2
else
    usage
fi

case "$DISTRO" in
    ubuntu|debian|kali)
        run_timed "$DISTRO $* ${INTERACTIVE_EXTRA_ARGS[*]}" "$ROOTFS_BUILDER" --distro "$DISTRO" "$@" "${INTERACTIVE_EXTRA_ARGS[@]}"
        ;;
    sailfish)
        if [ $# -eq 0 ]; then
            # No sub-action given (either the interactive picker, which
            # never had any trailing args to begin with, or a bare
            # --distro sailfish) -- "rpm" with no flags is build_packages.sh's
            # own "build everything" default, matching the debian-family
            # full-build default above.
            run_timed "sailfish rpm" "$SAILFISH_BUILDER" rpm
        else
            run_timed "sailfish $*" "$SAILFISH_BUILDER" "$@"
        fi
        ;;
    *)
        echo "Unknown distro: $DISTRO" >&2
        usage
        ;;
esac
