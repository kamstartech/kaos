#!/system/bin/sh
# Kaos Distro Configuration Library
# Location: /system/bin/distros-lib.sh
# Sourced by: kaos-service, kaos-starter, kaos-chroot-install
#
# Two modes:
# - HybridOS mode: /data/.stowaway/distros.conf exists → multi-distro support
# - Official Kali mode: No distros.conf → standard Kali paths at /data/local/kaos_system/

# === Constants ===
# distros.conf is seeded from /system/etc/distros.conf on first boot (init.kaos.rc).
# During init or if /data is not yet populated, fall back to the system copy.
DISTROS_CONF="/data/.stowaway/distros.conf"
if [ ! -f "$DISTROS_CONF" ] && [ -f "/system/etc/distros.conf" ]; then
    DISTROS_CONF="/system/etc/distros.conf"
fi
HYBRIDOS_BASE="/data/.stowaway"
HYBRIDOS_MOUNT="/data/rootfs"
OFFICIAL_KAOS_BASE="/data/local/kaos_system"
OFFICIAL_KAOS_REAL="kali-arm64"
OFFICIAL_KAOS_MOUNT="kalifs"

# === Resolved Variables (set by resolve_distro) ===
DISTRO_NAME=""
DISTRO_TYPE=""        # directory | image
DISTRO_PATH=""        # relative dir/image name
DISTRO_ARCH=""
DISTRO_SHELL=""
DISTRO_INIT=""
DISTRO_HOSTNAME=""
DISTRO_STATUS=""
DISTRO_URL=""
DISTRO_ARCHIVE_FMT=""
DISTRO_MODE=""        # hybridos | official (per-distro install mode)
DISTRO_VARIANT=""     # pro | "" — kali-pro needs cgroup delegation + full systemd
DISTRO_OFFICIAL_PATH="" # absolute official path (empty = hybridos-only distro)

# Resolved absolute paths
REAL_CHROOT=""        # actual rootfs location
CHROOT=""             # su mount point
PID_FILE=""
LOG_FILE=""
INPUT_PID_FILE=""
DISPLAY_BRIDGE_PID_FILE=""

# Preserve backward compatibility with older Kali aliases while keeping a
# single canonical distro identity in config and runtime state.
normalize_distro_name() {
    case "$1" in
        kali-pro|kali_native|kali-native)
            echo "kali"
            ;;
        *)
            echo "$1"
            ;;
    esac
}

# === INI Parser ===
# Reads a key from a section in distros.conf
# Usage: distro_get <section> <key>
distro_get() {
    local section="$1" key="$2"
    if [ ! -f "$DISTROS_CONF" ]; then
        return 1
    fi
    sed -n "/^\[$section\]/,/^\[/p" "$DISTROS_CONF" | grep "^${key}=" | head -1 | cut -d'=' -f2-
}

# === List Distros ===
# Returns all section names from distros.conf
distro_list() {
    if [ ! -f "$DISTROS_CONF" ]; then
        echo "kali"
        return 0
    fi
    grep '^\[' "$DISTROS_CONF" | tr -d '[]'
}

# === Active Distro ===
# Returns the distro pointed to by the 'active' symlink, or kali as default
distro_active() {
    if [ -L "${HYBRIDOS_BASE}/active" ]; then
        normalize_distro_name "$(basename "$(readlink "${HYBRIDOS_BASE}/active")" | sed 's/-arm64$//' | sed 's/-armhf$//')"
    elif [ ! -f "$DISTROS_CONF" ]; then
        echo "kali"
    else
        # First installed distro in conf
        local first
        for d in $(distro_list); do
            local status
            status=$(distro_get "$d" "status")
            if [ "$status" = "installed" ]; then
                echo "$d"
                return 0
            fi
        done
        echo "kali"
    fi
}

# === Resolve Distro ===
# Populates all DISTRO_* and path variables for a given distro name.
# Usage: resolve_distro [distro_name]
# If no name given, uses the active distro.
resolve_distro() {
    local distro
    distro="$(normalize_distro_name "${1:-$(distro_active)}")"
    DISTRO_NAME="$distro"

    if [ -f "$DISTROS_CONF" ]; then
        # Config exists — read distro properties
        DISTRO_TYPE=$(distro_get "$distro" "type")
        DISTRO_PATH=$(distro_get "$distro" "path")
        DISTRO_ARCH=$(distro_get "$distro" "arch")
        DISTRO_SHELL=$(distro_get "$distro" "shell")
        DISTRO_INIT=$(distro_get "$distro" "init")
        DISTRO_HOSTNAME=$(distro_get "$distro" "hostname")
        DISTRO_STATUS=$(distro_get "$distro" "status")
        DISTRO_URL=$(distro_get "$distro" "url")
        DISTRO_ARCHIVE_FMT=$(distro_get "$distro" "archive_format")
        DISTRO_MODE=$(distro_get "$distro" "mode")
        DISTRO_VARIANT=$(distro_get "$distro" "variant")
        DISTRO_OFFICIAL_PATH=$(distro_get "$distro" "official_path")

        # Defaults
        : "${DISTRO_TYPE:=directory}"
        : "${DISTRO_ARCH:=arm64}"
        : "${DISTRO_SHELL:=/bin/bash}"
        # Init resolution: use distros.conf value if set, otherwise auto-detect
        if [ -z "$DISTRO_INIT" ]; then
            DISTRO_INIT="__auto__"
        fi
        : "${DISTRO_HOSTNAME:=$distro}"
        : "${DISTRO_MODE:=hybridos}"

        # Resolve absolute paths based on per-distro mode
        if [ "$DISTRO_MODE" = "official" ] && [ -n "$DISTRO_OFFICIAL_PATH" ]; then
            REAL_CHROOT="$DISTRO_OFFICIAL_PATH"
            # Mount point varies by distro's official convention
            case "$distro" in
                kali)  CHROOT="${OFFICIAL_KAOS_BASE}/${OFFICIAL_KAOS_MOUNT}" ;;
                *)     CHROOT="$DISTRO_OFFICIAL_PATH" ;;
            esac
        else
            DISTRO_MODE="hybridos"
            REAL_CHROOT="${HYBRIDOS_BASE}/${DISTRO_PATH}"
            CHROOT="$HYBRIDOS_MOUNT"
        fi
    else
        # No config — fallback to standard Kali paths
        DISTRO_MODE="official"
        DISTRO_NAME="kali"
        DISTRO_TYPE="directory"
        DISTRO_PATH="$OFFICIAL_KAOS_REAL"
        DISTRO_ARCH="arm64"
        DISTRO_SHELL="/bin/bash"
        DISTRO_INIT="/usr/local/sbin/kaos-init"
        DISTRO_HOSTNAME="kali"
        DISTRO_STATUS="installed"
        DISTRO_URL="https://kali.download/nethunter-images/current/rootfs"
        DISTRO_ARCHIVE_FMT="tar.xz"
        DISTRO_OFFICIAL_PATH="${OFFICIAL_KAOS_BASE}/${OFFICIAL_KAOS_REAL}"

        REAL_CHROOT="${OFFICIAL_KAOS_BASE}/${OFFICIAL_KAOS_REAL}"
        CHROOT="${OFFICIAL_KAOS_BASE}/${OFFICIAL_KAOS_MOUNT}"
    fi

    # Auto-detect init system if not explicitly set
    if [ "$DISTRO_INIT" = "__auto__" ]; then
        if [ -x "${REAL_CHROOT}/sbin/init" ] || [ -L "${REAL_CHROOT}/sbin/init" ]; then
            # Distro has its own init (systemd, openrc, etc.) — use it
            DISTRO_INIT="/sbin/init"
        elif [ -x "${REAL_CHROOT}/usr/lib/systemd/systemd" ]; then
            DISTRO_INIT="/usr/lib/systemd/systemd"
        elif [ -x "${REAL_CHROOT}/usr/local/sbin/kaos-init" ]; then
            # No native init — fall back to our custom init script
            DISTRO_INIT="/usr/local/sbin/kaos-init"
        else
            DISTRO_INIT="/bin/bash"
        fi
    fi

    # Common derived paths
    PID_FILE="/data/local/tmp/kaos-${DISTRO_NAME}.pid"
    INPUT_PID_FILE="/data/local/tmp/kaos-input.pid"
    DISPLAY_BRIDGE_PID_FILE="/data/local/tmp/kaos-display-bridge.pid"
    LOG_FILE="/data/adb/kaos-service.log"
}

# === Distro-Aware Logging ===
# Prefixes every message with [datetime] [distro]
# Usage: dlog "message"
dlog() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] [${DISTRO_NAME:-unknown}] $1"
    echo "$msg" >> "${LOG_FILE:-/data/adb/kaos-service.log}"
    echo "$msg"
}

# === Mount Helpers ===
# Mount a distro's rootfs to the mount point
# For directory type: bind mount
# For image type: loop mount
distro_mount() {
    local target="$CHROOT"

    if [ "$DISTRO_TYPE" = "image" ]; then
        # Loop mount ext4 image
        if ! mountpoint -q "$target" 2>/dev/null; then
            mkdir -p "$target"
            local loop_dev
            loop_dev=$(losetup -f)
            losetup "$loop_dev" "$REAL_CHROOT"
            mount -t ext4 "$loop_dev" "$target"
            dlog "Loop mounted ${DISTRO_PATH} on ${target} via ${loop_dev}"
        fi
    else
        # Bind mount directory
        if ! mountpoint -q "$target" 2>/dev/null; then
            mkdir -p "$target"
            mount --bind "$REAL_CHROOT" "$target"
            dlog "Bind mounted ${REAL_CHROOT} on ${target}"
        fi
    fi
}

# Unmount distro rootfs
distro_umount() {
    local target="$CHROOT"

    if mountpoint -q "$target" 2>/dev/null; then
        umount "$target" 2>/dev/null
        dlog "Unmounted ${target}"
    fi

    # Release loop device if image type
    if [ "$DISTRO_TYPE" = "image" ]; then
        local loop_dev
        loop_dev=$(losetup -j "$REAL_CHROOT" 2>/dev/null | cut -d: -f1)
        if [ -n "$loop_dev" ]; then
            losetup -d "$loop_dev"
            dlog "Released loop device ${loop_dev}"
        fi
    fi
}

# === Busybox Finder ===
# Searches common locations, adapts path based on mode.
# busybox_kaos is installed to /system/bin/ by the ROM build (prebuilts/Android.mk).
find_busybox() {
    local extra_path=""
    if [ "$DISTRO_MODE" = "official" ]; then
        extra_path="${OFFICIAL_KAOS_BASE}/${OFFICIAL_KAOS_MOUNT}/bin/busybox"
    else
        extra_path="${HYBRIDOS_MOUNT}/bin/busybox"
    fi

    for path in /system/bin/busybox_kaos /system/xbin/busybox_kaos "$extra_path" /system/bin/busybox /vendor/bin/busybox; do
        if [ -x "$path" ]; then
            echo "$path"
            return 0
        fi
    done
    return 1
}

# === Validate Distro ===
# Check if a distro name exists in config
distro_exists() {
    local distro
    distro="$(normalize_distro_name "$1")"
    if [ ! -f "$DISTROS_CONF" ]; then
        [ "$distro" = "kali" ] && return 0 || return 1
    fi
    grep -q "^\[$distro\]" "$DISTROS_CONF"
}

# === Parse --distro Flag ===
# Extracts distro name from command args, returns remaining args
# Usage: eval $(parse_distro_flag "$@")
# Sets: _DISTRO_ARG and _REMAINING_ARGS
parse_distro_flag() {
    _DISTRO_ARG=""
    _REMAINING_ARGS=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --distro)
                shift
                _DISTRO_ARG="$(normalize_distro_name "$1")"
                ;;
            *)
                _REMAINING_ARGS="${_REMAINING_ARGS} $1"
                ;;
        esac
        shift
    done
    _REMAINING_ARGS=$(echo "$_REMAINING_ARGS" | sed 's/^ //')
}
