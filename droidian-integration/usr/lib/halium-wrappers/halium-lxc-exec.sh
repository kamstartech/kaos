#!/bin/bash

error() {
	echo "E: $@" >&2
	exit 1
}

TARGET_BINARY="${0/android_/}"
LXC_CONTAINER_NAME="android"
# HYBRIDOS PATCH (2026-08-25): upstream halium-wrappers hardcodes
# /var/lib/lxc/${LXC_CONTAINER_NAME}/rootfs, which is the upstream Halium
# default.  The Kaos/HybridOS ROM uses a system-as-root (SAR) partition
# layout and deliberately sets the LXC guest rootfs path to /android
# (see kaos/apps/phosh-app/res/raw/lxc_generate_dynamic_config.sh and
# lxc_pre_start.sh).  This override is a consequence of that project-wide
# architectural choice, not an upstream bug.
LXC_CONTAINER_PATH="/android"
# Second fix, same root cause: upstream's third search path is
# "${LXC_CONTAINER_PATH}/rootfs/vendor/bin/" -- correct ONLY when
# LXC_CONTAINER_PATH itself already ends in ".../android/rootfs" (upstream's
# default), where it resolves to that same directory's own "rootfs/vendor"
# subpath. With LXC_CONTAINER_PATH now "/android" directly, leaving this
# unchanged would expand to the nonexistent "/android/rootfs/vendor/bin/"
# instead of the real "/android/vendor/bin/" (confirmed live, matches the
# sibling system/bin and system/xbin entries' own pattern -- no extra
# "rootfs/" nesting under /android on this ROM).
ANDROID_SEARCH_PATH="${LXC_CONTAINER_PATH}/system/bin ${LXC_CONTAINER_PATH}/system/xbin ${LXC_CONTAINER_PATH}/vendor/bin/"

########################################################################

[ "${UID}" == 0 ] || error "This wrapper must be run from root"
[ -e "${LXC_CONTAINER_PATH}" ] || error "Unable to find LXC container"

found_path=$(whereis -b -B ${ANDROID_SEARCH_PATH} -f ${TARGET_BINARY} | head -n 1 | awk '{ print $2 }')

[ -n "${found_path}" ] || error "Unable to find ${TARGET_BINARY}"

# Unset eventual LD_PRELOAD
unset LD_PRELOAD

# On some devices, last argument is removed by lxc-attach
if test -f /var/lib/droidian/lxc_attach_workaround ; then
    exec /usr/bin/lxc-attach -n ${LXC_CONTAINER_NAME} -- ${found_path/${LXC_CONTAINER_PATH}/} ${@} ""
else
    exec /usr/bin/lxc-attach -n ${LXC_CONTAINER_NAME} -- ${found_path/${LXC_CONTAINER_PATH}/} ${@}
fi
