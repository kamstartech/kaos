#!/system/bin/sh
# sailfish-fixup.sh — Fix SailfishOS rootfs mount units for the current device
# The SailfishOS rootfs ships with OnePlus 6 (enchilada) partition paths.
# This script detects the current device and rewrites the systemd mount units
# to use the correct block device paths.
#
# Usage: sailfish-fixup.sh <rootfs_path>
# Example: sailfish-fixup.sh /data/.stowaway/sailfish
#
# Runs automatically on first namespace start (creates .fixup_done marker).

ROOTFS="${1:-/data/.stowaway/sailfish}"
UNITS="$ROOTFS/usr/lib/systemd/system"
MARKER="$ROOTFS/.fixup_done"
LOG_TAG="sailfish-fixup"
FIXUP_VERSION="5"
FORCE="$2"

log() { echo "[$LOG_TAG] $*"; }

run_rootfs() {
    chroot "$ROOTFS" /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin /bin/sh -c "$1"
}

# Graphics stack fixup: Mesa (freedreno/KGSL) instead of libhybris.
# We keep this function to verify Mesa is present, but no longer replace it.
needs_graphics_fix() {
    local egl="$ROOTFS/usr/lib64/libEGL.so.1.0.0"
    [ -f "$egl" ] || return 1
    strings "$egl" 2>/dev/null | grep -Eq 'MESA_platform_gbm|libgbm\.so\.1|freedreno'
}

fix_graphics_stack() {
    if needs_graphics_fix; then
        log "Graphics stack uses Mesa EGL (freedreno)."
        return 0
    fi

    log "WARNING: Mesa EGL not detected in rootfs."
    log "If you intended to use Mesa, ensure mesa-freedreno-libEGL is installed."
    return 0
}

fix_compositor_env() {
    local dir="$ROOTFS/var/lib/environment/compositor"
    local file="$dir/droid-hal-device.conf"

    mkdir -p "$dir" || return 1
    cat > "$file" <<'EOF'
# Perseus (Mi Mix 3) — Mesa freedreno KMS/DRM rendering (Adreno 630)
# Stack: Qt eglfs_kms → Mesa libEGL → GBM → /dev/dri/card0 → DSI-1
# droid-hal-startup.sh stubs Android graphics services to prevent conflicts.
QT_QPA_PLATFORM=eglfs
QT_QPA_EGLFS_INTEGRATION=eglfs_kms
QT_QPA_EGLFS_KMS_CONFIG=/etc/qt-kms.json
QT_QPA_EGLFS_KMS_ATOMIC=1
QT_QPA_EGLFS_NO_LIBINPUT=1
EGL_PLATFORM=gbm
QT_DEVICE_PIXEL_RATIO=1.5
LIPSTICK_OPTIONS=-plugin evdevtouch -plugin evdevkeyboard
QT_QPA_EGLFS_DISABLE_INPUT=1
MESA_LOADER_DRIVER_OVERRIDE=zink
TU_DEBUG=startup
MESA_DEBUG=1
LIBGL_DEBUG=verbose
VK_LOADER_DEBUG=all
QT_LOGGING_RULES=qt.qpa.*=true;org.nemomobile.lipstick.*=true;QWayland*=true
EOF
    log "Updated compositor environment for Mesa KMS: $file"
}

fix_mce_compositor() {
    # Keep Mesa-KMS MCE config (used for DRM DPMS control).
    local mesa_kms="$ROOTFS/etc/mce/70-compositor-mesa-kms.ini"
    if [ -f "$mesa_kms" ]; then
        log "Mesa-KMS MCE config present: $mesa_kms"
    else
        log "WARNING: Mesa-KMS MCE config missing at $mesa_kms"
    fi

    # Remove legacy hwcomposer MCE config if present.
    local hwc_conf="$ROOTFS/etc/mce/60-compositor-perseus.ini"
    if [ -f "$hwc_conf" ]; then
        rm -f "$hwc_conf"
        log "Removed legacy hwcomposer MCE config: $hwc_conf"
    fi
}

if [ -f "$MARKER" ] && [ "$FORCE" != "--force" ]; then
    if grep -q "^version=$FIXUP_VERSION\$" "$MARKER" 2>/dev/null && needs_graphics_fix; then
        log "Already applied (marker version $FIXUP_VERSION). Use --force to re-run."
        exit 0
    fi
    log "Marker is stale or graphics stack changed; re-running fixups."
fi

if [ ! -d "$UNITS" ]; then
    log "ERROR: Mount units directory not found at $UNITS"
    exit 1
fi

# Detect device from Android properties or by-name symlinks
DEVICE=$(getprop ro.product.device 2>/dev/null)
log "Device: ${DEVICE:-unknown}"

# Build partition map from /dev/block/platform/*/by-name/ symlinks (used for raw partitions only)
BY_NAME=""
for d in /dev/block/platform/*/by-name /dev/block/platform/*/*/by-name; do
    [ -d "$d" ] && BY_NAME="$d" && break
done

if [ -z "$BY_NAME" ]; then
    log "ERROR: Cannot find by-name partition directory"
    exit 1
fi

log "Partition directory: $BY_NAME"

# resolve_raw: non-dynamic partitions (persist, modem, dsp, bluetooth) — real raw block devices
resolve_raw() {
    local name="$1"
    local link=$(readlink "$BY_NAME/$name" 2>/dev/null)
    [ -z "$link" ] && echo "" && return
    echo "$link" | sed 's|^\.\./\.\./||; s|^|/dev/block/|' | sed 's|/dev/block//dev/block/|/dev/block/|'
}

MODEM_DEV=$(resolve_raw modem)
DSP_DEV=$(resolve_raw dsp)
BLUETOOTH_DEV=$(resolve_raw bluetooth)
PERSIST_DEV=$(resolve_raw persist)

log "Resolved raw partitions:"
log "  modem/firmware=$MODEM_DEV dsp=$DSP_DEV"
log "  bluetooth=$BLUETOOTH_DEV persist=$PERSIST_DEV"

# install_droid_mount_setup: installs the runtime DM path detection script and service.
#
# Dynamic partitions (system, vendor, product, system_ext, odm) live in the "super"
# block as DM logical devices. Their /dev/ paths differ between Android context
# (where fixup runs: /dev/block/mapper/ or /dev/block/dm-N) and the SailfishOS cold
# boot context (switch_root: /dev/mapper/ and /dev/dm-N). Hardcoding paths at fixup
# time causes mount failures on cold boot.
#
# Solution: defer path detection to a systemd service that runs inside the SFOS
# environment where the correct paths are known, then create /run/droid/<name>
# symlinks that mount units reference.
install_droid_mount_setup() {
    cat > "$ROOTFS/usr/bin/droid-mount-setup.sh" << 'SETUP'
#!/bin/sh
# droid-mount-setup.sh — Detect Android dynamic partition device paths at boot time.
# Tries multiple candidate paths and creates /run/droid/<name> symlinks for mount units.
# Logs every attempt to /dev/kmsg and /var/log/droid-mount-setup.log.

RUNDIR=/run/droid
LOGFILE=/var/log/droid-mount-setup.log

mkdir -p "$RUNDIR"
mkdir -p "$(dirname "$LOGFILE")"

log() {
    local msg="[droid-mount-setup] $*"
    echo "$msg" >> "$LOGFILE"
    echo "$msg" > /dev/kmsg 2>/dev/null
}

log "=== START (kernel: $(uname -r)) ==="
log "--- /dev/block/mapper/ ---"
ls /dev/block/mapper/ >> "$LOGFILE" 2>&1 || log "  (empty or missing)"
log "--- /dev/block/dm-* ---"
ls /dev/block/dm-* >> "$LOGFILE" 2>&1 || log "  (none)"
log "--- /dev/mapper/ ---"
ls /dev/mapper/ >> "$LOGFILE" 2>&1 || log "  (none)"
log "--- /dev/dm-* ---"
ls /dev/dm-* >> "$LOGFILE" 2>&1 || log "  (none)"

setup_partition() {
    local name="$1"
    local symlink="$RUNDIR/$name"

    log "--- $name ---"

    # Try 1: /dev/block/mapper/<name> — Android userspace DM symlink
    for cand in "/dev/block/mapper/$name" "/dev/block/mapper/${name}_a"; do
        if [ -b "$cand" ]; then
            ln -sf "$cand" "$symlink"
            log "OK $name -> $cand  [/dev/block/mapper]"
            return 0
        fi
        log "  MISS: $cand"
    done

    # Try 2: sysfs dm/name match -> /dev/block/dm-N (Android kernel path)
    for dm_path in /sys/block/dm-*; do
        [ -d "$dm_path" ] || continue
        dm_name=$(cat "$dm_path/dm/name" 2>/dev/null)
        if [ "$dm_name" = "$name" ] || [ "$dm_name" = "${name}_a" ]; then
            local blk="/dev/block/$(basename "$dm_path")"
            if [ -b "$blk" ]; then
                ln -sf "$blk" "$symlink"
                log "OK $name -> $blk  [sysfs dm/name=$dm_name]"
                return 0
            fi
            log "  MISS sysfs match dm_name=$dm_name but $blk not a block dev"
        fi
    done

    # Try 3: /dev/mapper/<name> — present in SFOS cold boot (switch_root) environment
    for cand in "/dev/mapper/$name" "/dev/mapper/${name}_a"; do
        if [ -b "$cand" ]; then
            ln -sf "$cand" "$symlink"
            log "OK $name -> $cand  [/dev/mapper]"
            return 0
        fi
        log "  MISS: $cand"
    done

    # Try 4: /dev/dm-N via sysfs — last resort legacy path
    for dm_path in /sys/block/dm-*; do
        [ -d "$dm_path" ] || continue
        dm_name=$(cat "$dm_path/dm/name" 2>/dev/null)
        if [ "$dm_name" = "$name" ] || [ "$dm_name" = "${name}_a" ]; then
            local dm_dev="/dev/$(basename "$dm_path")"
            if [ -b "$dm_dev" ]; then
                ln -sf "$dm_dev" "$symlink"
                log "OK $name -> $dm_dev  [/dev/dm-N legacy]"
                return 0
            fi
            log "  MISS /dev/dm-N: $dm_dev not a block dev"
        fi
    done

    log "FAIL: no device found for $name — mount will fail"
    return 1
}

setup_partition system
setup_partition vendor
setup_partition product
setup_partition odm
setup_partition system_ext

log "--- /run/droid/ ---"
ls -la "$RUNDIR/" >> "$LOGFILE" 2>&1
log "=== DONE ==="
exit 0
SETUP
    chmod 755 "$ROOTFS/usr/bin/droid-mount-setup.sh"

    cat > "$UNITS/droid-mount-setup.service" << 'SVC'
[Unit]
Description=Detect Android droid partition device paths
DefaultDependencies=no
Before=local-fs.target system_root.mount vendor.mount product.mount odm.mount system_ext.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/droid-mount-setup.sh

[Install]
WantedBy=local-fs.target
SVC

    mkdir -p "$UNITS/local-fs.target.wants"
    ln -sf "../droid-mount-setup.service" "$UNITS/local-fs.target.wants/droid-mount-setup.service"
    log "Installed droid-mount-setup.sh and service"
}

# fix_unit_dynamic: rewrites a DM partition mount unit to use /run/droid/<name>.
# The actual device is resolved at boot time by droid-mount-setup.service.
fix_unit_dynamic() {
    local unit="$1"
    local part_name="$2"
    local mnt_point="$3"
    local fstype="${4:-ext4}"
    local options="${5:-ro}"
    local extra_before="${6:-}"
    local file="$UNITS/$unit"

    cat > "$file" << EOF
[Unit]
Description=Droid mount for $mnt_point
DefaultDependencies=no
After=droid-mount-setup.service
Requires=droid-mount-setup.service
Before=local-fs.target systemd-modules-load.service${extra_before:+ $extra_before}

[Mount]
What=/run/droid/$part_name
Where=$mnt_point
Type=$fstype
Options=$options
TimeoutSec=10

[Install]
WantedBy=local-fs.target
EOF
    mkdir -p "$UNITS/local-fs.target.wants"
    if [ ! -e "$UNITS/local-fs.target.wants/$unit" ]; then
        ln -sf "../$unit" "$UNITS/local-fs.target.wants/$unit"
    fi
    log "  FIXED $unit: What=/run/droid/$part_name (runtime detection)"
}

# fix_unit_raw: updates or creates a raw (non-DM) partition mount unit.
fix_unit_raw() {
    local unit="$1"
    local new_what="$2"
    local mnt_point="$3"
    local file="$UNITS/$unit"

    if [ -z "$new_what" ]; then
        log "  SKIP $unit (no device resolved)"
        return
    fi

    if [ ! -f "$file" ]; then
        log "  CREATING $unit for $mnt_point..."
        cat > "$file" << EOF
[Unit]
Description=Droid mount for $mnt_point
DefaultDependencies=no
Before=local-fs.target systemd-modules-load.service

[Mount]
What=$new_what
Where=$mnt_point
Type=ext4
Options=ro,barrier=1,discard
TimeoutSec=10

[Install]
WantedBy=local-fs.target
EOF
        mkdir -p "$UNITS/local-fs.target.wants"
        ln -sf "../$unit" "$UNITS/local-fs.target.wants/$unit"
        return
    fi

    local old_what=$(grep '^What=' "$file" | head -1)
    if [ -n "$old_what" ]; then
        sed -i "s|^What=.*|What=$new_what|" "$file"
        log "  FIXED $unit: $old_what → What=$new_what"
    fi
}

log "Installing droid-mount-setup..."
install_droid_mount_setup

log "Fixing dynamic partition mount units..."
# Dynamic partitions: paths detected at runtime inside SFOS by droid-mount-setup.service
fix_unit_dynamic "system_root.mount" "system"      "/system_root" "ext4" "ro,barrier=1,discard"
fix_unit_dynamic "vendor.mount"      "vendor"      "/vendor"      "ext4" "ro" "vendor-bt_firmware.mount vendor-dsp.mount vendor-firmware_mnt.mount"
fix_unit_dynamic "product.mount"     "product"     "/product"     "ext4" "ro"
fix_unit_dynamic "odm.mount"         "odm"         "/odm"         "ext4" "ro"
fix_unit_dynamic "system_ext.mount"  "system_ext"  "/system_ext"  "ext4" "ro"

log "Fixing raw partition mount units..."
fix_unit_raw "vendor-firmware_mnt.mount" "$MODEM_DEV"     "/vendor/firmware_mnt"
fix_unit_raw "vendor-dsp.mount"          "$DSP_DEV"        "/vendor/dsp"
fix_unit_raw "vendor-bt_firmware.mount"  "$BLUETOOTH_DEV"  "/vendor/bt_firmware"
fix_unit_raw "mnt-vendor-persist.mount"  "$PERSIST_DEV"    "/mnt/vendor/persist"

# system.mount: bind mount of /system_root/system — path is always static
if [ -f "$UNITS/system.mount" ]; then
    sed -i "s|^What=.*|What=/system_root/system|" "$UNITS/system.mount"
    log "  FIXED system.mount bind path"
fi

# Fix MCE compositor config (rename from enchilada to perseus)
if [ -f "$ROOTFS/etc/mce/60-compositor-enchilada.ini" ]; then
    log "Fixing MCE compositor config..."
    mv "$ROOTFS/etc/mce/60-compositor-enchilada.ini" "$ROOTFS/etc/mce/60-compositor-perseus.ini"
    # Ensure service name and setprop path are correct for our build
    sed -i 's|vendor.hwcomposer-2-3|vendor.hwcomposer-2-3|g' "$ROOTFS/etc/mce/60-compositor-perseus.ini"
    sed -i 's|/system/bin/setprop|/usr/bin/setprop|g' "$ROOTFS/etc/mce/60-compositor-perseus.ini"
fi

# Create mount point directories if missing
for mp in system_root system system_ext product vendor odm vendor/firmware_mnt vendor/dsp \
           vendor/bt_firmware mnt/vendor/persist config; do
    mkdir -p "$ROOTFS/$mp" 2>/dev/null
done

fix_compositor_env || exit 1
fix_graphics_stack || exit 1

# Write marker with device info
{
    echo "version=$FIXUP_VERSION"
    echo "device=$DEVICE"
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
} > "$MARKER"
log "Done. Marker written to $MARKER"
