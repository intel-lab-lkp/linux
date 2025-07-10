#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

###############################################################################
#
# Input subsystem tracepoint testing script
#
# AUTHOR
#      WangYuli <wangyuli@uniontech.com>
#
###############################################################################

DEBUGFS_PATH="/sys/kernel/debug/tracing"
INPUT_EVENTS_PATH="${DEBUGFS_PATH}/events/input"

# Check if we have sufficient privileges
if [ ! -w "$DEBUGFS_PATH" ]; then
    echo "Error: Root privileges required to access tracing system"
    echo "Please run: sudo $0"
    exit 1
fi

# Check if debugfs is mounted
if [ ! -d "$DEBUGFS_PATH" ]; then
    echo "Error: debugfs is not mounted"
    echo "Please run: mount -t debugfs none /sys/kernel/debug"
    exit 1
fi

# Check if input tracepoints exist
if [ ! -d "$INPUT_EVENTS_PATH" ]; then
    echo "Error: input tracepoints not found, kernel may need to be recompiled"
    exit 1
fi

echo "=== Input Subsystem Tracepoint Test ==="
echo

# Clear existing trace buffer
echo > "${DEBUGFS_PATH}/trace"

# List available input tracepoints
echo "Available Input Tracepoints:"
for event in "${INPUT_EVENTS_PATH}"/*; do
    if [ -d "$event" ]; then
        event_name=$(basename "$event")
        echo "  - $event_name"
    fi
done
echo

# Enable all input tracepoints
echo "Enabling all input tracepoints..."
echo 1 > "${INPUT_EVENTS_PATH}/enable"

if [ $? -eq 0 ]; then
    echo "✓ Successfully enabled input tracepoints"
else
    echo "✗ Failed to enable input tracepoints"
    exit 1
fi

echo
echo "Please perform some operations in another terminal (keyboard input, mouse movement, etc.)"
echo "or plug/unplug USB devices, then come back to view the results..."
echo
echo "Press any key to continue viewing trace output (press Ctrl+C to exit)..."
read -n 1

echo
echo "=== Trace Output ==="
echo "(Last 100 lines)"
tail -n 100 "${DEBUGFS_PATH}/trace"

echo
echo "=== Real-time Trace Output ==="
echo "(Press Ctrl+C to stop)"
cat "${DEBUGFS_PATH}/trace_pipe"
