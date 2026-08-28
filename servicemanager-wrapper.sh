#!/bin/sh
# Hybris servicemanager wrapper: logs output, bypasses selinux_status_open errors
# Uses SailfishOS logging (logger)

LOGGER_TAG="servicemanager-wrapper"
logger -t "$LOGGER_TAG" "Starting servicemanager wrapper: $@"

# Exec the real servicemanager, capturing output
exec /system/bin/servicemanager "$@" 2>&1 | while read line; do
    logger -t "$LOGGER_TAG" "$line"
done
