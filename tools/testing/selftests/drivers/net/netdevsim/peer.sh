#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

NSIM_DEV_1_ID=$((RANDOM % 1024))
NSIM_DEV_1_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_1_ID
NSIM_DEV_1_DFS=/sys/kernel/debug/netdevsim/netdevsim$NSIM_DEV_1_ID
NSIM_DEV_2_ID=$((RANDOM % 1024))
NSIM_DEV_2_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_2_ID
NSIM_DEV_2_DFS=/sys/kernel/debug/netdevsim/netdevsim$NSIM_DEV_2_ID

NSIM_DEV_SYS_NEW=/sys/bus/netdevsim/new_device
NSIM_DEV_SYS_DEL=/sys/bus/netdevsim/del_device

socat_check()
{
	if [ ! -x "$(command -v socat)" ]; then
		echo "socat command not found. Skipping test"
		return 1
	fi

	return 0
}

setup_ns()
{
	set -e
	ip netns add nssv
	ip netns add nscl

	NSIM_DEV_1_NAME=$(find $NSIM_DEV_1_SYS/net -maxdepth 1 -type d ! \
		-path $NSIM_DEV_1_SYS/net -exec basename {} \;)
	NSIM_DEV_2_NAME=$(find $NSIM_DEV_2_SYS/net -maxdepth 1 -type d ! \
		-path $NSIM_DEV_2_SYS/net -exec basename {} \;)

	ip link set $NSIM_DEV_1_NAME netns nssv
	ip link set $NSIM_DEV_2_NAME netns nscl

	ip netns exec nssv ip addr add '192.168.1.1/24' dev $NSIM_DEV_1_NAME
	ip netns exec nscl ip addr add '192.168.1.2/24' dev $NSIM_DEV_2_NAME

	ip netns exec nssv ip link set dev $NSIM_DEV_1_NAME up
	ip netns exec nscl ip link set dev $NSIM_DEV_2_NAME up
	set +e
}

cleanup_ns()
{
	ip netns del nscl
	ip netns del nssv
}

###
### Code start
###

modprobe netdevsim

# linking

echo $NSIM_DEV_1_ID > $NSIM_DEV_SYS_NEW

echo "$NSIM_DEV_2_ID 0" > ${NSIM_DEV_1_DFS}/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with non-existent netdevsim should fail"
	exit 1
fi

echo $NSIM_DEV_2_ID > $NSIM_DEV_SYS_NEW

echo "$NSIM_DEV_2_ID 0" > ${NSIM_DEV_1_DFS}/ports/0/peer
if [ $? -ne 0 ]; then
	echo "linking netdevsim1 port0 with netdevsim2 port0 should succeed"
	exit 1
fi

# argument error checking

echo "$NSIM_DEV_2_ID 1" > ${NSIM_DEV_1_DFS}/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with non-existent port in a netdevsim should fail"
	exit 1
fi

echo "$NSIM_DEV_1_ID 0" > ${NSIM_DEV_1_DFS}/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with self should fail"
	exit 1
fi

echo "$NSIM_DEV_2_ID a" > ${NSIM_DEV_1_DFS}/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "invalid arg should fail"
	exit 1
fi

# send/recv packets

socat_check || exit 4

setup_ns

tmp_file=$(mktemp)
ip netns exec nssv socat TCP-LISTEN:1234,fork $tmp_file &
pid=$!

echo "HI" | ip netns exec nscl socat STDIN TCP:192.168.1.1:1234

count=$(cat $tmp_file | wc -c)
if [[ $count -ne 3 ]]; then
	echo "expected 3 bytes, got $count"
	exit 1
fi

echo $NSIM_DEV_2_ID > $NSIM_DEV_SYS_DEL

kill $pid
echo $NSIM_DEV_1_ID > $NSIM_DEV_SYS_DEL

cleanup_ns

modprobe -r netdevsim

exit 0
