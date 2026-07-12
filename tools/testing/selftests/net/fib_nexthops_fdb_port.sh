#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Control-plane selftest for per-nexthop VXLAN fdb destination port
# (NHA_FDB_PORT).  Verifies the accept/reject rules and the dump roundtrip.
# No datapath traffic here -- see tests/integ/vxlan-fdb-port-integ.sh for the
# real forwarding test.
#
# Requires: patched kernel (NHA_FDB_PORT) and patched iproute2 (the "port"
# keyword on "ip nexthop ... fdb").  SKIPs cleanly otherwise.

set -u

ksft_skip=4
NS="nhfdbport-$$"
IP="ip -netns $NS"
ret=0

log_test() {	# $1 actual_rc  $2 expected_rc  $3 name
	if [ "$1" = "$2" ]; then
		printf "TEST: %-58s [ OK ]\n" "$3"
	else
		printf "TEST: %-58s [FAIL] (rc=$1 want=$2)\n" "$3"
		ret=1
	fi
}

# passes (returns 0) iff the command FAILS
expect_fail() {
	if "$@" >/dev/null 2>&1; then return 1; else return 0; fi
}

cleanup() { ip netns del "$NS" 2>/dev/null; }

command -v ip >/dev/null 2>&1 || { echo "SKIP: iproute2 not found"; exit $ksft_skip; }
ip nexthop help 2>&1 | grep -q fdb || { echo "SKIP: no fdb nexthop support"; exit $ksft_skip; }

trap cleanup EXIT
cleanup
ip netns add "$NS" || { echo "SKIP: cannot create netns"; exit $ksft_skip; }
$IP link set lo up

# Probe for "port" keyword + kernel NHA_FDB_PORT support; SKIP if missing.
if ! $IP nexthop add id 1 via 10.0.0.1 fdb port 4790 2>/dev/null; then
	echo "SKIP: 'ip nexthop ... fdb port' unsupported (needs patched kernel + iproute2)"
	exit $ksft_skip
fi
log_test 0 0 "add fdb nexthop with port"

# Dump roundtrip must echo the port back.
$IP nexthop show id 1 | grep -qw "port 4790"
log_test $? 0 "dump shows fdb port 4790"

# Reject: port on a routed (non-fdb) nexthop.
expect_fail $IP nexthop add id 2 via 10.0.0.1 dev lo port 4790
log_test $? 0 "reject port on non-fdb nexthop"

# Reject: fdb port without a gateway.
expect_fail $IP nexthop add id 3 fdb port 4790
log_test $? 0 "reject fdb port without gateway"

# The HA case: a group whose legs share the gateway but differ in port.
$IP nexthop add id 10 via 10.0.0.1 fdb port 4789 && \
$IP nexthop add id 11 via 10.0.0.1 fdb port 5789 && \
$IP nexthop add id 100 group 10/11 fdb
log_test $? 0 "add fdb nexthop group with differing ports"

# A fdb nexthop without a port must NOT emit one (backward compat).
$IP nexthop add id 20 via 10.0.0.1 fdb
$IP nexthop show id 20 | grep -qw "port"
log_test $? 1 "fdb nexthop without port omits NHA_FDB_PORT"

if [ $ret -eq 0 ]; then
	echo "PASS: all NHA_FDB_PORT control-plane checks"
else
	echo "FAIL: one or more NHA_FDB_PORT checks failed"
fi
exit $ret
