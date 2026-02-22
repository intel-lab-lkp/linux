#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2022 Meta Platforms, Inc. and affiliates.

source ../lib.sh

NS_NAME="ns-"$RANDOM
TEAM_NAME="team-"$RANDOM
DUMMY_NAME="dummy-"$RANDOM

cleanup() {
    cleanup_ns $NS_NAME
    ip link del $TEAM_NAME
}

trap cleanup EXIT
ip link add name $TEAM_NAME type team
ip link add name $DUMMY_NAME mtu 1499 master $TEAM_NAME type dummy
ip netns add $NS_NAME
echo "Setting $DUMMY_NAME to $NS_NAME"
ip link set dev $DUMMY_NAME netns $NS_NAME
echo "Deleting $DUMMY_NAME from $NS_NAME"
ip -n $NS_NAME link del dev $DUMMY_NAME
echo "Test completed successfully."
