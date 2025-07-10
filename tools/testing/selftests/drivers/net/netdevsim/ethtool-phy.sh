#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

source ethtool-common.sh

# Bail if ethtool is too old
if ! ethtool -h | grep show-phys >/dev/null 2>&1; then
    echo "SKIP: No --show-phys support in ethtool"
    exit 4
fi

function make_netdev_from_id {
    local new_nsim_id="$1"
    # Make a netdevsim
    echo "$new_nsim_id" > /sys/bus/netdevsim/new_device
    udevadm settle
    # get new device name
    ls /sys/bus/netdevsim/devices/netdevsim"${new_nsim_id}"/net/
}

function cleanup_netdev_from_id {
    local to_del_nsim_id="$1"
    echo "$to_del_nsim_id" > /sys/bus/netdevsim/del_device
}

NSIM_NETDEV=$(make_netdev)

set -o pipefail

# Check simple PHY addition and listing

# Parent == 0 means that the PHY's parent is the netdev
PHY_DFS=$(make_phydev_on_netdev "$NSIM_ID" 0)

# First PHY gets index 1
index=$(ethtool --show-phys "$NSIM_NETDEV" | grep "PHY index" | cut -d ' ' -f 3)
check $? "$index" "1"

# Insert a second PHY, same parent. It gets index 2.
PHY2_DFS=$(make_phydev_on_netdev "$NSIM_ID" 0)

# Create another netdev
NSIM_ID2=$((RANDOM % 1024))
NSIM_NETDEV_2=$(make_netdev_from_id "$NSIM_ID2")

PHY3_DFS=$(make_phydev_on_netdev "$NSIM_ID2" 0);

# Check unfiltered PHY Dump
n_phy=$(ethtool --show-phys '*' | grep -c "PHY index")
check $? "$n_phy" "3"

# Check filtered Dump
n_phy=$(ethtool --show-phys "$NSIM_NETDEV" | grep -c "PHY index")
check $? "$n_phy" "2"

cleanup_netdev_from_id "$NSIM_ID2"

if [ "$num_errors" -eq 0 ]; then
    echo "PASSED all $((num_passes)) checks"
    exit 0
else
    echo "FAILED $num_errors/$((num_errors+num_passes)) checks"
    exit 1
fi
