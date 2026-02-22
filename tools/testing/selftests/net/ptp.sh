#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
source lib.sh

NSIM_DEV_1_ID=$((256 + RANDOM % 256))
NSIM_DEV_2_ID=$((512 + RANDOM % 256))

NSIM_DEV_SYS_LINK=/sys/bus/netdevsim/link_device
NSIM_DEV_SYS_UNLINK=/sys/bus/netdevsim/unlink_device
PTP4L_SYNC_TIMEOUT=20

setup_netdevsim()
{
	set -e
	ip netns add nssv
	ip netns add nscl

	NSIM_DEV_1_NAME=$(create_netdevsim $NSIM_DEV_1_ID nssv)
	NSIM_DEV_2_NAME=$(create_netdevsim $NSIM_DEV_2_ID nscl)

	ip netns exec nssv ip addr add '192.168.1.1/24' dev $NSIM_DEV_1_NAME
	ip netns exec nscl ip addr add '192.168.1.2/24' dev $NSIM_DEV_2_NAME

	set +e
}

cleanup()
{
	[ -n "${PTP4L_LEADER_PID-}" ] && {
		kill $PTP4L_LEADER_PID $PTP4L_FOLLOWER_PID 2>/dev/null
		wait $PTP4L_LEADER_PID $PTP4L_FOLLOWER_PID 2>/dev/null
		rm -f "$PTP4L_LEADER_LOG" "$PTP4L_FOLLOWER_LOG"
	}

	[ -n "${NSIM_DEV_1_FD-}" ] && [ -n "${NSIM_DEV_1_IFIDX-}" ] && \
		echo "$NSIM_DEV_1_FD:$NSIM_DEV_1_IFIDX" > "$NSIM_DEV_SYS_UNLINK" 2>/dev/null
	cleanup_netdevsim "${NSIM_DEV_2_ID-}" 2>/dev/null
	cleanup_netdevsim "${NSIM_DEV_1_ID-}" 2>/dev/null

	ip netns del nscl 2>/dev/null
	ip netns del nssv 2>/dev/null

	modprobe -r netdevsim 2>/dev/null
}

###
### Code start
###
if [ ! -x "$(command -v ptp4l)" ]; then
	echo "ptp4l command not found. Skipping PTP sync test"
	exit 4
fi

trap cleanup EXIT

setup_netdevsim

# Link netdevsim1 with netdevsim2
NSIM_DEV_1_FD=$((256 + RANDOM % 256))
exec {NSIM_DEV_1_FD}</var/run/netns/nssv
NSIM_DEV_1_IFIDX=$(ip netns exec nssv cat /sys/class/net/$NSIM_DEV_1_NAME/ifindex)

NSIM_DEV_2_FD=$((512 + RANDOM % 256))
exec {NSIM_DEV_2_FD}</var/run/netns/nscl
NSIM_DEV_2_IFIDX=$(ip netns exec nscl cat /sys/class/net/$NSIM_DEV_2_NAME/ifindex)

if ! echo "$NSIM_DEV_1_FD:$NSIM_DEV_1_IFIDX $NSIM_DEV_2_FD:$NSIM_DEV_2_IFIDX" > "$NSIM_DEV_SYS_LINK"; then
	echo "linking netdevsim1 with netdevsim2 failed"
	exit 1
fi

# PTP synchronization test: run ptp4l leader in nssv and follower in nscl,
# then parse follower output to verify they synchronized (servo state s2 = locked).
PTP4L_LEADER_LOG=$(mktemp)
PTP4L_FOLLOWER_LOG=$(mktemp)
ip netns exec nssv ptp4l -i "$NSIM_DEV_1_NAME" -m -4 -P \
	> "$PTP4L_LEADER_LOG" 2>&1 &
PTP4L_LEADER_PID=$!

ip netns exec nscl ptp4l -i "$NSIM_DEV_2_NAME" -s -m -4 -P \
	> "$PTP4L_FOLLOWER_LOG" 2>&1 &
PTP4L_FOLLOWER_PID=$!


for _ in $(seq 1 $PTP4L_SYNC_TIMEOUT); do
	if grep -q ' s2 ' "$PTP4L_FOLLOWER_LOG" 2>/dev/null; then
		exit 0
	fi
	sleep 1
done

echo "ptp4l follower did not reach locked state (s2) within ${PTP4L_SYNC_TIMEOUT}s"
exit 1