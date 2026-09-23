#!/bin/bash
# Task 13 / TRD-044 drift check — closes the `/kaos.init` + `kaos-init` keep-in-sync
# duplication in the UNIFY-DISTRO-SUPPORT-ENGINE plan (F3/Task 13).
#
# One content oracle, two ship sites. The sparse tree copy is the oracle; the
# phosh-app res/raw mirror must byte-match it or the build FAILS on drift
# (instead of silently shipping a stale warm init that breaks the namespace
# PID1 once — the failure this check is designed to make loud and early).
#
# Idempotent: exit 0 when in sync, 1 when drifted. Read-only; never writes.
#
# Identity contract (both currently carry this marker verbatim):
#   "Shared, byte-identical logic used two ways in this project"
set -u

ORACLE="${KAOS_ORACLE:-hybris/droid-configs/sparse/kaos.init}"
MIRROR="${KAOS_MIRROR:-kaos/apps/phosh-app/res/raw/droid_dm_setup.sh}"
NONFATAL_DRIFT_OK="${KAOS_NONFATAL_DRIFT_OK:-0}"

# Use cmp exit status; do NOT rely on `cmp -s` short-circuit wording.
if ! cmp -s "$ORACLE" "$MIRROR"; then
    echo "KAOS-DRIFT: $ORACLE and $MIRROR are no longer byte-identical." >&2
    echo "           They are the same keep-in-sync pair (keep-in-sync header)." >&2
    echo "           Re-run the oracle: cp '$MIRROR' '$ORACLE' (or vice versa)." >&2
    if [ "$NONFATAL_DRIFT_OK" = "1" ]; then
        echo "KAOS-DRIFT: nonfatal mode, not failing the build." >&2
        exit 0
    fi
    exit 1
fi
echo "kaos.init/kaos-init drift check: in sync ($ORACLE)"
exit 0
