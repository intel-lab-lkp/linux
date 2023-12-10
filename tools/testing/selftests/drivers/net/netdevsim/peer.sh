#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

NSIM_DEV_SYS=/sys/bus/netdevsim
NSIM_DEV_DFS=/sys/kernel/debug/netdevsim/netdevsim

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

	ip link set eni1np1 netns nssv
	ip link set eni2np1 netns nscl

	ip netns exec nssv ip addr add '192.168.1.1/24' dev eni1np1
	ip netns exec nscl ip addr add '192.168.1.2/24' dev eni2np1

	ip netns exec nssv ip link set dev eni1np1 up
	ip netns exec nscl ip link set dev eni2np1 up
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

echo 1 > ${NSIM_DEV_SYS}/new_device

echo "2 0" > ${NSIM_DEV_DFS}1/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with non-existent netdevsim should fail"
	exit 1
fi

echo 2 > /sys/bus/netdevsim/new_device

echo "2 0" > ${NSIM_DEV_DFS}1/ports/0/peer
if [ $? -ne 0 ]; then
	echo "linking netdevsim1 port0 with netdevsim2 port0 should succeed"
	exit 1
fi

# argument error checking

echo "2 1" > ${NSIM_DEV_DFS}1/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with non-existent port in a netdevsim should fail"
	exit 1
fi

echo "1 0" > ${NSIM_DEV_DFS}1/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "linking with self should fail"
	exit 1
fi

echo "2 a" > ${NSIM_DEV_DFS}1/ports/0/peer 2>/dev/null
if [ $? -eq 0 ]; then
	echo "invalid arg should fail"
	exit 1
fi

# send/recv packets

socat_check || exit 1

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

echo 2 > ${NSIM_DEV_SYS}/del_device

kill $pid
echo 1 > ${NSIM_DEV_SYS}/del_device

cleanup_ns

modprobe -r netdevsim

exit 0
