#!/bin/bash
# Download Kali NetHunter chroot images
# Updated Jan 10, 2026 with correct upstream filenames

CHROOT_DIR="$(dirname "$0")"
DOWNLOAD_URL_BASE="https://kali.download/nethunter-images/current/rootfs"

# Available chroot variants (Corrected filenames)
declare -A CHROOT_VARIANTS=(
    ["minimal"]="kali-nethunter-rootfs-minimal-arm64.tar.xz"
    ["nano"]="kali-nethunter-rootfs-nano-arm64.tar.xz"
    ["full"]="kali-nethunter-rootfs-full-arm64.tar.xz"
)

# Default to minimal (smallest at 132MB)
VARIANT="${1:-minimal}"

if [[ ! -v "CHROOT_VARIANTS[$VARIANT]" ]]; then
    echo "Invalid variant: $VARIANT"
    echo "Available variants: ${!CHROOT_VARIANTS[@]}"
    exit 1
fi

REMOTE_FILE="${CHROOT_VARIANTS[$VARIANT]}"
# We want to keep our local naming convention for the build system
LOCAL_FILE="kalifs-arm64-$VARIANT.tar.xz"
DOWNLOAD_URL="$DOWNLOAD_URL_BASE/$REMOTE_FILE"

echo "Downloading Kali NetHunter chroot: $VARIANT"
echo "URL: $DOWNLOAD_URL"
echo ""

cd "$CHROOT_DIR"

# Download
echo "Downloading..."
wget -c "$DOWNLOAD_URL" -O "$LOCAL_FILE"

if [ $? -eq 0 ]; then
    # Download checksum (Remote filename + .sha512sum)
    echo "Downloading checksum..."
    wget -c "$DOWNLOAD_URL.sha512sum" -O "$LOCAL_FILE.sha512sum"
    
    # Note: Upstream uses sha512sum now
    # We need to adjust the checksum file content to match our local filename
    sed -i "s|$REMOTE_FILE|$LOCAL_FILE|" "$LOCAL_FILE.sha512sum"

    # Verify checksum
    echo "Verifying checksum..."
    sha512sum -c "$LOCAL_FILE.sha512sum"
    
    if [ $? -eq 0 ]; then
        echo "✓ Download complete and verified: $LOCAL_FILE"
        ls -lh "$LOCAL_FILE"
    else
        echo "✗ Checksum verification failed!"
        exit 1
    fi
else
    echo "✗ Download failed!"
    exit 1
fi
