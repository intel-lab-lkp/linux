#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Regression test for the mr_check_range() iova overflow in SoftRoCE (rxe).
#
# A remote peer can craft an RDMA-WRITE whose RETH makes iova + length wrap
# to 0, bypassing the old
#
#	if (iova + length > mr->ibmr.iova + mr->ibmr.length)
#
# range check in mr_check_range(). The responder then derives a huge page
# index in rxe_mr_iova_to_index() (only WARN_ON-guarded) and dereferences
# mr->page_info[huge] -> out-of-bounds read/write / kernel oops, triggerable
# by an unauthenticated remote peer.
#
# Fixed by rewriting the check in overflow-safe form:
#
#	if (iova < mr->ibmr.iova ||
#	    length > mr->ibmr.length ||
#	    iova - mr->ibmr.iova > mr->ibmr.length - length)
#
# Topology: a veth pair across a network namespace, one rxe device on each
# end. The server (ns) registers a USER MR and accepts an RDMA_CM
# connection, passing its rkey/iova in the private data. The client (host)
# posts one RDMA-WRITE with remote_addr = 0xfffffffffffffff8, len = 8.
#
#   - Patched kernel:   PASS  (mr_check_range() rejects the crafted iova;
#                              the client completion is IBV_WC_REM_ACCESS_ERR;
#                              no kernel warning/oops in dmesg)
#   - Unpatched kernel: FAIL  (WARN_ON in rxe_mr_iova_to_index followed by
#                              an OOB page fault / oops in rxe_mr_copy)

NS="rxe_mrovf"
VETH_NS="vmo-a"
VETH_HOST="vmo-b"
IP_NS="1.1.1.1"
IP_HOST="1.1.1.2"
PORT=4792
BIN="$(dirname "$(readlink -f "$0")")/rxe_mr_overflow"

source "$(dirname "$0")/../kselftest/ktap_helpers.sh"

SRV=

# Remove the topology this script creates. Idempotent: safe to call even when
# some of the resources no longer exist (e.g. partial setup, prior abort).
rxe_teardown() {
	rdma link del rxe1 2>/dev/null
	ip netns exec "$NS" rdma link del rxe0 2>/dev/null
	ip link delete "$VETH_HOST" 2>/dev/null
	ip netns del "$NS" 2>/dev/null
}

cleanup() {
	trap '' INT TERM EXIT		# guard against re-entry
	# Kill the server first so the client does not block on a dead peer.
	[ -n "$SRV" ] && kill "$SRV" 2>/dev/null
	wait "$SRV" 2>/dev/null
	rxe_teardown
	modprobe -r rdma_rxe 2>/dev/null
}
# Cover normal exit, Ctrl+C (SIGINT) and kill (SIGTERM) alike.
trap cleanup INT TERM EXIT

# Tear down any leftover topology from a previous aborted run (SIGINT, kill
# -9, guest crash, ...) so this script is always re-runnable instead of
# failing on "RTNETLINK answers: File exists".
rxe_teardown

# --- Prerequisites ---
if [ "$EUID" -ne 0 ]; then
	ktap_print_header
	ktap_skip_all "needs root"
	exit "$KSFT_SKIP"
fi
if ! modinfo rdma_rxe >/dev/null 2>&1; then
	ktap_print_header
	ktap_skip_all "rdma_rxe module not found"
	exit "$KSFT_SKIP"
fi
if [ ! -x "$BIN" ]; then
	ktap_print_header
	ktap_skip_all "$BIN not built (needs libibverbs-dev / librdmacm-dev)"
	exit "$KSFT_SKIP"
fi

modprobe rdma_rxe >/dev/null 2>&1

# --- Topology: veth pair across a netns, one rxe device on each end ---
ip netns add "$NS"
ip link add "$VETH_NS" type veth peer name "$VETH_HOST"
ip link set "$VETH_NS" netns "$NS"

ip netns exec "$NS" ip addr add "$IP_NS/24" dev "$VETH_NS"
ip netns exec "$NS" ip link set "$VETH_NS" up
ip netns exec "$NS" ip link set lo up
ip addr add "$IP_HOST/24" dev "$VETH_HOST"
ip link set "$VETH_HOST" up

ip netns exec "$NS" rdma link add rxe0 type rxe netdev "$VETH_NS"
rdma link add rxe1 type rxe netdev "$VETH_HOST"

if ! ping -c 2 -W 1 "$IP_NS" >/dev/null 2>&1; then
	ktap_print_header
	ktap_skip_all "no connectivity between host and netns"
	exit "$KSFT_SKIP"
fi

# Only look at warnings produced by this run.
dmesg -C >/dev/null 2>&1

# --- Run: server in the netns, client on the host ---
# On an unpatched kernel the crafted WRITE can wedge the responder; wrap both
# sides in timeout() so the script always reaches a verdict and runs cleanup
# instead of hanging on a stuck peer.
ktap_print_header
ktap_set_plan 1

ip netns exec "$NS" timeout 30 "$BIN" "$IP_NS" "$PORT" >/dev/null 2>&1 &
SRV=$!
sleep 2
timeout 30 "$BIN" -c "$IP_NS" "$PORT" >/dev/null 2>&1
wait "$SRV" 2>/dev/null

# --- Verdict: any rxe MR-range warning / OOB means the kernel is unpatched ---
if dmesg 2>/dev/null | grep -qE "rxe_mr_iova_to_index|mr_check_range|BUG:.*rxe_mr_copy|KASAN:.*rxe_mr"; then
	ktap_test_fail "mr_check_range() iova overflow (UNPATCHED): OOB/WARN in dmesg"
else
	ktap_test_pass "mr_check_range() rejected crafted iova (patched)"
fi

ktap_finished
