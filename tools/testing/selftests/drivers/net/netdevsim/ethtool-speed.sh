#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

source ethtool-common.sh

NSIM_NETDEV=$(make_netdev)

set -o pipefail

s=$(ethtool --json "$NSIM_NETDEV" | jq '.[].speed')
check $? "$s" "5000"

s=$(ethtool --json "$NSIM_NETDEV" | jq -r '.[].duplex')
check $? "$s" "Full"

ethtool -s "$NSIM_NETDEV" speed 1000 duplex half

s=$(ethtool --json "$NSIM_NETDEV" | jq '.[].speed')
check $? "$s" "1000"

s=$(ethtool --json "$NSIM_NETDEV" | jq -r '.[].duplex')
check $? "$s" "Half"

ethtool -s "$NSIM_NETDEV" speed 10000 2>/dev/null
check $? "" "" 1

if [ "$num_errors" -eq 0 ]; then
    echo "PASSED all $((num_passes)) checks"
    exit 0
else
    echo "FAILED $num_errors/$((num_errors+num_passes)) checks"
    exit 1
fi
