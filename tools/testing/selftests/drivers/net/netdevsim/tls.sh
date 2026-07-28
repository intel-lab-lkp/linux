#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Exercise netdevsim's emulated kTLS device offload over a linked
# netdevsim pair, one port per network namespace.
#
# shellcheck disable=SC2154 # ksft_skip comes from lib.sh

lib_dir=$(dirname "$0")
# shellcheck source=./../../../net/lib.sh
# shellcheck disable=SC1091
source "$lib_dir"/../../../net/lib.sh

NSIM_DEV_1_ID=$((256 + RANDOM % 256))
NSIM_DEV_1_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_1_ID
NSIM_DEV_2_ID=$((512 + RANDOM % 256))
NSIM_DEV_2_SYS=/sys/bus/netdevsim/devices/netdevsim$NSIM_DEV_2_ID

NSIM_DEV_SYS_NEW=/sys/bus/netdevsim/new_device
NSIM_DEV_SYS_DEL=/sys/bus/netdevsim/del_device
NSIM_DEV_SYS_LINK=/sys/bus/netdevsim/link_device

DEBUGFS=/sys/kernel/debug/netdevsim
NSIM_DEV_1_TLS=$DEBUGFS/netdevsim$NSIM_DEV_1_ID/ports/0/tls
NSIM_DEV_2_TLS=$DEBUGFS/netdevsim$NSIM_DEV_2_ID/ports/0/tls

SRV_IP=192.168.13.1
CLI_IP=192.168.13.2
PORT=4433

SYNCDIR=
BIN=$lib_dir/tls_offload

num_pass=0
num_fail=0

check()
{
	local msg="$1"
	local ret="$2"

	if [ "$ret" -eq 0 ]; then
		echo "PASS: $msg"
		num_pass=$((num_pass + 1))
	else
		echo "FAIL: $msg"
		num_fail=$((num_fail + 1))
	fi
}

# shellcheck disable=SC2317,SC2329 # invoked from the EXIT trap
cleanup()
{
	ip netns del nscl 2>/dev/null
	ip netns del nssv 2>/dev/null
	echo "$NSIM_DEV_2_ID" > "$NSIM_DEV_SYS_DEL" 2>/dev/null
	echo "$NSIM_DEV_1_ID" > "$NSIM_DEV_SYS_DEL" 2>/dev/null
	[ -n "$SYNCDIR" ] && rm -rf "$SYNCDIR"
}

setup()
{
	set -e

	echo "$NSIM_DEV_1_ID" > "$NSIM_DEV_SYS_NEW"
	echo "$NSIM_DEV_2_ID" > "$NSIM_DEV_SYS_NEW"
	udevadm settle 2>/dev/null || sleep 1

	NSIM_DEV_1_NAME=$(find "$NSIM_DEV_1_SYS"/net -maxdepth 1 -type d ! \
		-path "$NSIM_DEV_1_SYS"/net -exec basename {} \;)
	NSIM_DEV_2_NAME=$(find "$NSIM_DEV_2_SYS"/net -maxdepth 1 -type d ! \
		-path "$NSIM_DEV_2_SYS"/net -exec basename {} \;)

	ip netns add nssv
	ip netns add nscl

	ip link set "$NSIM_DEV_1_NAME" netns nssv
	ip link set "$NSIM_DEV_2_NAME" netns nscl

	ip netns exec nssv ip addr add "$SRV_IP/24" dev "$NSIM_DEV_1_NAME"
	ip netns exec nscl ip addr add "$CLI_IP/24" dev "$NSIM_DEV_2_NAME"

	ip netns exec nssv ip link set dev "$NSIM_DEV_1_NAME" up
	ip netns exec nscl ip link set dev "$NSIM_DEV_2_NAME" up

	NSIM_DEV_1_FD=$((256 + RANDOM % 256))
	exec {NSIM_DEV_1_FD}</var/run/netns/nssv
	NSIM_DEV_1_IFIDX=$(ip netns exec nssv \
		cat /sys/class/net/"$NSIM_DEV_1_NAME"/ifindex)

	NSIM_DEV_2_FD=$((256 + RANDOM % 256))
	exec {NSIM_DEV_2_FD}</var/run/netns/nscl
	NSIM_DEV_2_IFIDX=$(ip netns exec nscl \
		cat /sys/class/net/"$NSIM_DEV_2_NAME"/ifindex)

	echo "$NSIM_DEV_1_FD:$NSIM_DEV_1_IFIDX $NSIM_DEV_2_FD:$NSIM_DEV_2_IFIDX" \
		> "$NSIM_DEV_SYS_LINK"

	SYNCDIR=$(mktemp -d)
	set +e
}

feature()
{
	local netns="$1"
	local dev="$2"
	local feat="$3"

	ip netns exec "$netns" ethtool -k "$dev" 2>/dev/null | \
		sed -n "s/^$feat: \([a-z]*\).*/\1/p"
}

dbg_field()
{
	sed -n "s/.*\<$2=\([0-9]*\).*/\1/p" "$1" | head -1
}

tls_stat()
{
	ip netns exec "$1" cat /proc/net/tls_stat | \
		sed -n "s/^$2 \([0-9]*\)/\1/p"
}

# Both ends park once their offload is installed and before any data is
# sent, so the driver's context count can be sampled without racing the
# transfer.  Pass "nosample" when the offload is expected to be refused,
# since then neither end ever reaches the barrier.
run_pair()
{
	local sample="${1:-sample}"
	local srv_rc cli_rc waited=0

	rm -f "$SYNCDIR"/*.ready "$SYNCDIR"/go
	CONNS_1=0
	CONNS_2=0

	ip netns exec nssv "$BIN" server "$SRV_IP" "$PORT" "$SYNCDIR" &
	local srv_pid=$!
	ip netns exec nscl "$BIN" client "$SRV_IP" "$PORT" "$SYNCDIR" &
	local cli_pid=$!

	if [ "$sample" = "sample" ]; then
		while [ ! -e "$SYNCDIR/server.ready" ] ||
		      [ ! -e "$SYNCDIR/client.ready" ]; do
			[ "$waited" -ge 200 ] && break
			sleep 0.05
			waited=$((waited + 1))
		done
		CONNS_1=$(dbg_field "$NSIM_DEV_1_TLS" count)
		CONNS_2=$(dbg_field "$NSIM_DEV_2_TLS" count)
		: "${CONNS_1:=0}"
		: "${CONNS_2:=0}"
	fi
	touch "$SYNCDIR/go"

	wait "$srv_pid"; srv_rc=$?
	wait "$cli_pid"; cli_rc=$?

	[ "$srv_rc" -eq 0 ] && [ "$cli_rc" -eq 0 ]
}

###
### Code start
###

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: need root"
	exit "$ksft_skip"
fi

if ! command -v ethtool >/dev/null; then
	echo "SKIP: ethtool not found"
	exit "$ksft_skip"
fi

if [ ! -x "$BIN" ]; then
	echo "SKIP: $BIN not built"
	exit "$ksft_skip"
fi

modprobe netdevsim 2>/dev/null
if [ ! -d /sys/bus/netdevsim ]; then
	echo "SKIP: netdevsim not available"
	exit "$ksft_skip"
fi

modprobe tls 2>/dev/null
if [ ! -e /proc/net/tls_stat ]; then
	echo "SKIP: kernel TLS not available"
	exit "$ksft_skip"
fi

trap cleanup EXIT
setup

# The offload has to be advertised, and on by default like the other
# netdevsim crypto offloads.
for f in tls-hw-tx-offload tls-hw-rx-offload; do
	[ "$(feature nssv "$NSIM_DEV_1_NAME" "$f")" = "on" ]
	check "$f advertised and on by default" $?
done

[ -e "$NSIM_DEV_1_TLS" ]
check "per-port debugfs tls file exists" $?

# Main data path run.
run_pair
check "offloaded TLS data transfer" $?

# Sampled at the barrier, so each port must be holding exactly the TX and
# the RX context of its own socket.
[ "$CONNS_1" -eq 2 ] && [ "$CONNS_2" -eq 2 ]
check "tx and rx contexts installed on both ports" $?

# Both ends must have gone through the device path, not the SW fallback.
[ "$(tls_stat nssv TlsTxDevice)" -ge 1 ] && \
	[ "$(tls_stat nssv TlsRxDevice)" -ge 1 ] && \
	[ "$(tls_stat nscl TlsTxDevice)" -ge 1 ] && \
	[ "$(tls_stat nscl TlsRxDevice)" -ge 1 ]
check "both ends used the device path" $?

[ "$(tls_stat nssv TlsTxSw)" -eq 0 ] && [ "$(tls_stat nscl TlsTxSw)" -eq 0 ]
check "no silent fallback to the software path" $?

[ "$(tls_stat nssv TlsDecryptError)" -eq 0 ] && \
	[ "$(tls_stat nscl TlsDecryptError)" -eq 0 ]
check "no decrypt errors" $?

# The driver must have seen the records go by in both directions.
[ "$(dbg_field "$NSIM_DEV_1_TLS" tx_packets)" -ge 1 ] && \
	[ "$(dbg_field "$NSIM_DEV_1_TLS" rx_packets)" -ge 1 ] && \
	[ "$(dbg_field "$NSIM_DEV_2_TLS" tx_packets)" -ge 1 ] && \
	[ "$(dbg_field "$NSIM_DEV_2_TLS" rx_packets)" -ge 1 ]
check "driver counted offloaded packets both ways" $?

# Sockets are closed by now, so every context must have been given back.
[ "$(dbg_field "$NSIM_DEV_1_TLS" count)" -eq 0 ] && \
	[ "$(dbg_field "$NSIM_DEV_2_TLS" count)" -eq 0 ]
check "all offload contexts released on close" $?

[ "$(tls_stat nssv TlsCurrTxDevice)" -eq 0 ] && \
	[ "$(tls_stat nssv TlsCurrRxDevice)" -eq 0 ]
check "no device contexts left behind" $?

# Turning the feature off has to make the offload refuse the connection;
# the test binary insists on the device path, so it must now fail.
ip netns exec nssv ethtool -K "$NSIM_DEV_1_NAME" tls-hw-tx-offload off
ip netns exec nssv ethtool -K "$NSIM_DEV_1_NAME" tls-hw-rx-offload off
ip netns exec nscl ethtool -K "$NSIM_DEV_2_NAME" tls-hw-tx-offload off
ip netns exec nscl ethtool -K "$NSIM_DEV_2_NAME" tls-hw-rx-offload off

run_pair nosample
rc=$?
[ "$rc" -ne 0 ]
check "offload declined once the feature is off" $?

[ "$(tls_stat nssv TlsTxSw)" -ge 1 ]
check "software path used when offload is off" $?

ip netns exec nssv ethtool -K "$NSIM_DEV_1_NAME" tls-hw-tx-offload on
ip netns exec nssv ethtool -K "$NSIM_DEV_1_NAME" tls-hw-rx-offload on
ip netns exec nscl ethtool -K "$NSIM_DEV_2_NAME" tls-hw-tx-offload on
ip netns exec nscl ethtool -K "$NSIM_DEV_2_NAME" tls-hw-rx-offload on

run_pair
check "offload works again after re-enabling" $?

echo
echo "passed: $num_pass failed: $num_fail"
[ "$num_fail" -eq 0 ] && exit 0
exit 1
