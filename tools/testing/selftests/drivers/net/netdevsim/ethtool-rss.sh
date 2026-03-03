#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Test RSS indirection table resize on channel count changes.
# Exercises ethtool_rxfh_indir_resize() and
# ethtool_rxfh_contexts_resize_all() via netdevsim.

source ethtool-common.sh

# Create device with 8 queues
NSIM_NETDEV=$(make_netdev 1 8)

set -o pipefail

# --- Test 1: Default table regenerates on channel change ---
s=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"] | length')
check $? "$s" "128" # roundup_pow_of_two(8) * 16 = 128

ethtool -L $NSIM_NETDEV combined 2
s=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"] | length')
check $? "$s" "32" # roundup_pow_of_two(2) * 16 = 32

ethtool -L $NSIM_NETDEV combined 8

# --- Test 2: Periodic context 0 table folds on channel reduction ---
ethtool -X $NSIM_NETDEV equal 2
s=$(ethtool --json -x $NSIM_NETDEV | jq '[.[0]["rss-indirection-table"][]] | max')
check $? "$s" "1"

ethtool -L $NSIM_NETDEV combined 2
s=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"] | length')
check $? "$s" "32"
s=$(ethtool --json -x $NSIM_NETDEV | jq '[.[0]["rss-indirection-table"][]] | max')
check $? "$s" "1"

# Grow back — should unfold
ethtool -L $NSIM_NETDEV combined 8
s=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"] | length')
check $? "$s" "128"
s=$(ethtool --json -x $NSIM_NETDEV | jq '[.[0]["rss-indirection-table"][]] | max')
check $? "$s" "1"

ethtool -X $NSIM_NETDEV default

# --- Test 3: Non-periodic context 0 table blocks fold ---
ethtool -X $NSIM_NETDEV equal 8

ethtool -L $NSIM_NETDEV combined 2 2>/dev/null
if [ $? -ne 0 ]; then
    ((num_passes++))
else
    echo "Expected channel reduction to fail with non-periodic table"
    ((num_errors++))
fi

ethtool -X $NSIM_NETDEV default

# --- Test 4: Non-default context resizes on channel change ---
ctx_id=$(ethtool -X $NSIM_NETDEV context new equal 2 2>/dev/null | awk '{print $NF}')
if [ -z "$ctx_id" ]; then
    echo "SKIP: context creation failed"
else
    s=$(ethtool --json -x $NSIM_NETDEV context $ctx_id | jq '.[0]["rss-indirection-table"] | length')
    check $? "$s" "128"

    ethtool -L $NSIM_NETDEV combined 2
    s=$(ethtool --json -x $NSIM_NETDEV context $ctx_id | jq '.[0]["rss-indirection-table"] | length')
    check $? "$s" "32"
    s=$(ethtool --json -x $NSIM_NETDEV context $ctx_id | jq '[.[0]["rss-indirection-table"][]] | max')
    check $? "$s" "1"

    # Grow back
    ethtool -L $NSIM_NETDEV combined 8
    s=$(ethtool --json -x $NSIM_NETDEV context $ctx_id | jq '.[0]["rss-indirection-table"] | length')
    check $? "$s" "128"
    s=$(ethtool --json -x $NSIM_NETDEV context $ctx_id | jq '[.[0]["rss-indirection-table"][]] | max')
    check $? "$s" "1"

    ethtool -X $NSIM_NETDEV context $ctx_id delete
fi

# --- Test 5: Non-periodic non-default context blocks fold ---
ctx_id=$(ethtool -X $NSIM_NETDEV context new equal 8 2>/dev/null | awk '{print $NF}')
if [ -z "$ctx_id" ]; then
    echo "SKIP: context creation failed"
else
    ethtool -L $NSIM_NETDEV combined 2 2>/dev/null
    if [ $? -ne 0 ]; then
        ((num_passes++))
    else
        echo "Expected channel reduction to fail with non-periodic context"
        ((num_errors++))
    fi

    ethtool -X $NSIM_NETDEV context $ctx_id delete
fi

# --- Test 6: Table unchanged after failed channel reduction ---
ethtool -X $NSIM_NETDEV equal 8
s_before=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"]')

ethtool -L $NSIM_NETDEV combined 2 2>/dev/null
s_after=$(ethtool --json -x $NSIM_NETDEV | jq '.[0]["rss-indirection-table"]')
check $? "$s_after" "$s_before"

ethtool -X $NSIM_NETDEV default

# --- Test 7: Channel count unchanged after failed reduction ---
ethtool -X $NSIM_NETDEV equal 8
ethtool -L $NSIM_NETDEV combined 2 2>/dev/null
s=$(ethtool --json -l $NSIM_NETDEV | jq '.[0]["combined-count"]')
check $? "$s" "8"

ethtool -X $NSIM_NETDEV default

# --- Results ---
if [ $num_errors -eq 0 ]; then
    echo "PASSED all $((num_passes)) checks"
    exit 0
else
    echo "FAILED $num_errors/$((num_errors+num_passes)) checks"
    exit 1
fi
