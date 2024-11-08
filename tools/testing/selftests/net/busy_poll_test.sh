#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
source net_helper.sh

NSIM_DEV_1_ID=$((256 + RANDOM % 256))
NSIM_DEV_1_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_1_ID
NSIM_DEV_2_ID=$((512 + RANDOM % 256))
NSIM_DEV_2_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_2_ID

NSIM_DEV_SYS_NEW=/sys/bus/netdevsim/new_device
NSIM_DEV_SYS_DEL=/sys/bus/netdevsim/del_device
NSIM_DEV_SYS_LINK=/sys/bus/netdevsim/link_device
NSIM_DEV_SYS_UNLINK=/sys/bus/netdevsim/unlink_device

setup_ns()
{
	set -e
	ip netns add nssv
	ip netns add nscl

	NSIM_DEV_1_NAME=$(find $NSIM_DEV_1_SYS/net -maxdepth 1 -type d ! \
		-path $NSIM_DEV_1_SYS/net -exec basename {} \;)
	NSIM_DEV_2_NAME=$(find $NSIM_DEV_2_SYS/net -maxdepth 1 -type d ! \
		-path $NSIM_DEV_2_SYS/net -exec basename {} \;)

	# ensure the server has 1 queue
	ethtool -L $NSIM_DEV_1_NAME combined 1 2>/dev/null

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

test_busypoll()
{
	tmp_file=$(mktemp)
	out_file=$(mktemp)

	# fill a test file with random data
	dd if=/dev/urandom of=${tmp_file} bs=1M count=1 2> /dev/null

	timeout -k 1s 30s ip netns exec nssv ./busy_poller -p48675 -b192.168.1.1 -m8 -u0 -P1 -g16 -i${NSIM_DEV_1_IFIDX} -o${out_file}&

	wait_local_port_listen nssv 48675 tcp

	ip netns exec nscl socat -u $tmp_file TCP:192.168.1.1:48675

	wait

	tmp_file_md5sum=$(md5sum $tmp_file | cut -f1 -d' ')
	out_file_md5sum=$(md5sum $out_file | cut -f1 -d' ')

	if [ "$tmp_file_md5sum" = "$out_file_md5sum" ]; then
		res=0
	else
		echo "md5sum mismatch"
		echo "input file md5sum: ${tmp_file_md5sum}";
		echo "output file md5sum: ${out_file_md5sum}";
		res=1
	fi

	rm $out_file $tmp_file

	return $res
}

test_busypoll_with_suspend()
{
	tmp_file=$(mktemp)
	out_file=$(mktemp)

	# fill a test file with random data
	dd if=/dev/urandom of=${tmp_file} bs=1M count=1 2> /dev/null

	timeout -k 1s 30s ip netns exec nssv ./busy_poller -p48675 -b192.168.1.1 -m8 -u0 -P1 -g16 -d100 -r50000 -s20000000 -i${NSIM_DEV_1_IFIDX} -o${out_file}&

	wait_local_port_listen nssv 48675 tcp

	ip netns exec nscl socat -u $tmp_file TCP:192.168.1.1:48675

	wait

	tmp_file_md5sum=$(md5sum $tmp_file | cut -f1 -d' ')
	out_file_md5sum=$(md5sum $out_file | cut -f1 -d' ')

	if [ "$tmp_file_md5sum" = "$out_file_md5sum" ]; then
		res=0
	else
		echo "md5sum mismatch"
		echo "input file md5sum: ${tmp_file_md5sum}";
		echo "output file md5sum: ${out_file_md5sum}";
		res=1
	fi

	rm $out_file $tmp_file

	return $res
}

###
### Code start
###

modprobe netdevsim

# linking

echo $NSIM_DEV_1_ID > $NSIM_DEV_SYS_NEW
echo $NSIM_DEV_2_ID > $NSIM_DEV_SYS_NEW
udevadm settle

setup_ns

NSIM_DEV_1_FD=$((256 + RANDOM % 256))
exec {NSIM_DEV_1_FD}</var/run/netns/nssv
NSIM_DEV_1_IFIDX=$(ip netns exec nssv cat /sys/class/net/$NSIM_DEV_1_NAME/ifindex)

NSIM_DEV_2_FD=$((256 + RANDOM % 256))
exec {NSIM_DEV_2_FD}</var/run/netns/nscl
NSIM_DEV_2_IFIDX=$(ip netns exec nscl cat /sys/class/net/$NSIM_DEV_2_NAME/ifindex)

echo "$NSIM_DEV_1_FD:$NSIM_DEV_1_IFIDX $NSIM_DEV_2_FD:$NSIM_DEV_2_IFIDX" > $NSIM_DEV_SYS_LINK
if [ $? -ne 0 ]; then
	echo "linking netdevsim1 with netdevsim2 should succeed"
	cleanup_ns
	exit 1
fi

test_busypoll
if [ $? -ne 0 ]; then
	echo "test_busypoll failed"
	cleanup_ns
	exit 1
fi

test_busypoll_with_suspend
if [ $? -ne 0 ]; then
	echo "test_busypoll_with_suspend failed"
	cleanup_ns
	exit 1
fi

echo "$NSIM_DEV_1_FD:$NSIM_DEV_1_IFIDX" > $NSIM_DEV_SYS_UNLINK

echo $NSIM_DEV_2_ID > $NSIM_DEV_SYS_DEL

cleanup_ns

modprobe -r netdevsim

exit 0
