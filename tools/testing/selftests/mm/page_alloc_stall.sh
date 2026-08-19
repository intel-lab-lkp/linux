#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# page_alloc_stall.sh
#
# Orchestrator script to generate concurrent userspace and kernelspace
# memory allocation pressure over a synthetic zswap backing block device
# to artificially induce systemic memory contention.
#
# Dependencies:
# - External memtoy binary (Source: https://github.com/kosaki/memtoy)
#
# Usage:
# ./page_alloc_stall.sh [hog_percent=80] [workers=64] [overcommit=2.5] \
#                       [swap_gb=10%_RAM] [memtoy_path="memtoy/memtoy"]

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PYTHON_CMD=${PYTHON_CMD:-python3}

HOG_PERCENT=${1:-80}
NUM_WORKERS=${2:-64}
OVERCOMMIT=${3:-2.5}
TARGET_SWAP_GB=$4
MEMTOY_PATH=${5:-"memtoy/memtoy"}

function total_mem_gb()
{
    awk '/MemTotal/ {print int($2 / 1024 / 1024)}' /proc/meminfo
}

function enable_zswap_file()
{
    local swap_size_gb=$1

    fallocate -l ${swap_size_gb}G "$SCRIPT_DIR/swap.img"

    LOOP_DEV=$(losetup -f --show "$SCRIPT_DIR/swap.img")
    mkswap $LOOP_DEV
    swapon $LOOP_DEV

    echo 1 > /sys/module/zswap/parameters/enabled
}

function cleanup()
{
    echo "Tearing down the test..."
    if lsmod | grep -q test_mempress_timer; then
        rmmod test_mempress_timer || echo "Failed to rmmod mempress_timer"
    fi

    # Check if loop dev was actually created before trying to destroy it
    if [ -n "$LOOP_DEV" ]; then
        swapoff $LOOP_DEV 2>/dev/null || true
        losetup -d $LOOP_DEV 2>/dev/null || true
    fi
    rm -f "$SCRIPT_DIR/swap.img"
}

trap cleanup EXIT

if [ -z "$TARGET_SWAP_GB" ]; then
    # use 10% of the total mem for zswap file by default.
    TARGET_SWAP_GB=$(($(total_mem_gb) / 10))
fi

enable_zswap_file $TARGET_SWAP_GB

KROOT=$(cd "$SCRIPT_DIR/../../../.." && pwd)
KO_PATH="$KROOT/lib/test_mempress_timer.ko"

if [ "$KO_PATH" ]; then
    insmod "$KO_PATH"
else
    echo "WARNING: $KO_PATH not found! Aborting the test."
    exit 1
fi

"$PYTHON_CMD" "$SCRIPT_DIR/page_alloc_stall_pressure.py" $HOG_PERCENT $NUM_WORKERS $OVERCOMMIT $MEMTOY_PATH
