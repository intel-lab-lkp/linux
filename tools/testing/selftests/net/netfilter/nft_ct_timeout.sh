#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Functional test for nftables ct timeout objects.
#
# Verifies that a ct timeout policy actually changes the conntrack
# state-machine timeout of the connections it is attached to:
#
#  1. test_policy_applied    - a connection matching a "ct timeout set" rule
#                              gets the per-state timeout from the named
#                              policy, i.e. <= POLICY_ESTAB_SECS.
#
#  2. test_default_preserved - a connection not matching any ct timeout rule
#                              keeps the kernel default ESTABLISHED timeout.
#
#  3. test_policy_reload     - after the table is deleted and reloaded, a new
#                              connection through the rule still receives the
#                              policy timeout rather than the default.
#
# The remaining lifetime of a flow is the third field of a "conntrack -L"
# line:
#
#   tcp  6  117  ESTABLISHED  src=10.0.1.1 dst=10.0.1.2 sport=... dport=...
#           ^^^
#
# Topology: two netns connected by a veth pair.
#   ns1 (10.0.1.1/24) --veth-- ns2 (10.0.1.2/24)
# The ruleset is loaded in ns1, so ns1's conntrack table is used throughout.

source lib.sh

checktool "nft --version" "run test without nft"
checktool "conntrack --version" "run test without conntrack"
checktool "socat -h" "run test without socat"

# TCP port covered by the ct timeout policy.
PORT_POLICY=12345
# TCP port not covered by any rule; uses the kernel default timeouts.
PORT_DEFAULT=12346

# ESTABLISHED timeout carried by the policy, in seconds. Must be well below
# the kernel default (5 days) so the two cases cannot be confused.
POLICY_ESTAB_SECS=120

ret=0

cleanup()
{
	ip netns pids "$ns1" 2>/dev/null | xargs -r kill
	ip netns pids "$ns2" 2>/dev/null | xargs -r kill
	cleanup_all_ns
}

load_ruleset()
{
	ip netns exec "$ns1" nft -f - <<EOF
table ip ct_timeout_test {
	ct timeout tcp_policy {
		protocol tcp;
		policy = { established: ${POLICY_ESTAB_SECS}s };
	}

	chain output {
		type filter hook output priority filter; policy accept;
		ip daddr 10.0.1.2 tcp dport $PORT_POLICY ct timeout set "tcp_policy"
	}
}
EOF
}

listener_ready()
{
	ip netns exec "$ns2" ss -lnt -o "sport = :$1" | grep -q "$1"
}

conn_established()
{
	ip netns exec "$ns1" conntrack -L -p tcp --dport "$1" 2>/dev/null |
		grep -q ESTABLISHED
}

# Prints the remaining lifetime, in seconds, of the first ESTABLISHED TCP
# flow in ns1 matching the given destination port.
established_timeout()
{
	ip netns exec "$ns1" conntrack -L -p tcp --dport "$1" 2>/dev/null |
		awk '/ESTABLISHED/ { print $3; exit }'
}

# Opens a TCP connection from ns1 and holds it open in the background.
# Prints the pid of the holder so the caller can tear it down.
open_connection()
{
	# stdout/stderr must be redirected, else the background job keeps the
	# command substitution that calls this function blocked until it exits.
	ip netns exec "$ns1" bash -c "
		exec 3<>/dev/tcp/10.0.1.2/$1 || exit 1
		sleep 60
	" >/dev/null 2>&1 &
	echo $!
}

close_connection()
{
	kill "$1" 2>/dev/null
	wait "$1" 2>/dev/null
}

# Asserts that the flow on $1 has a timeout matching expectation $2 ("policy"
# or "default"), using $3 as the test name.
check_timeout()
{
	local dport=$1 expect=$2 name=$3
	local tval

	if ! busywait "$BUSYWAIT_TIMEOUT" conn_established "$dport"; then
		echo "FAIL: $name: connection did not reach ESTABLISHED"
		ret=1
		return
	fi

	tval=$(established_timeout "$dport")
	if [ -z "$tval" ]; then
		echo "FAIL: $name: could not read conntrack timeout"
		ret=1
		return
	fi

	if [ "$expect" = "policy" ]; then
		if [ "$tval" -le "$POLICY_ESTAB_SECS" ]; then
			echo "PASS: $name: timeout ${tval}s <= policy ${POLICY_ESTAB_SECS}s"
		else
			echo "FAIL: $name: timeout ${tval}s exceeds policy ${POLICY_ESTAB_SECS}s"
			ret=1
		fi
	else
		if [ "$tval" -gt "$POLICY_ESTAB_SECS" ] &&
		   [ "$tval" -le "$default_estab" ]; then
			echo "PASS: $name: timeout ${tval}s matches default ${default_estab}s"
		else
			echo "FAIL: $name: timeout ${tval}s is not the default ${default_estab}s"
			ret=1
		fi
	fi
}

trap cleanup EXIT

setup_ns ns1 ns2

if ! ip link add veth0 netns "$ns1" type veth peer name veth0 netns "$ns2" \
		>/dev/null 2>&1; then
	echo "SKIP: No virtual ethernet pair device support in kernel"
	exit $ksft_skip
fi

ip -net "$ns1" link set veth0 up
ip -net "$ns2" link set veth0 up
ip -net "$ns1" addr add 10.0.1.1/24 dev veth0
ip -net "$ns2" addr add 10.0.1.2/24 dev veth0

# SYSTEM:"cat" keeps each accepted connection open: cat blocks reading the
# socket, so the flow stays ESTABLISHED until the client goes away. Sinking
# to /dev/null instead would close it immediately and land in CLOSE_WAIT.
ip netns exec "$ns2" socat TCP-LISTEN:$PORT_POLICY,reuseaddr,fork SYSTEM:"cat" &>/dev/null &
ip netns exec "$ns2" socat TCP-LISTEN:$PORT_DEFAULT,reuseaddr,fork SYSTEM:"cat" &>/dev/null &

busywait "$BUSYWAIT_TIMEOUT" listener_ready "$PORT_POLICY"
busywait "$BUSYWAIT_TIMEOUT" listener_ready "$PORT_DEFAULT"

# Loading the ruleset pulls in conntrack, so the sysctls below exist only
# after this point.
if ! load_ruleset; then
	echo "SKIP: Could not load ct timeout ruleset"
	exit $ksft_skip
fi

# Read the default rather than hardcoding it: a host that lowered
# nf_conntrack_tcp_timeout_established must not fail test 2 spuriously.
default_estab=$(ip netns exec "$ns1" \
	cat /proc/sys/net/netfilter/nf_conntrack_tcp_timeout_established \
	2>/dev/null)

if [ -z "$default_estab" ]; then
	echo "SKIP: conntrack tcp timeout sysctl not available"
	exit $ksft_skip
fi

if [ "$default_estab" -le "$POLICY_ESTAB_SECS" ]; then
	echo "SKIP: default ESTABLISHED timeout ${default_estab}s is not above the policy value"
	exit $ksft_skip
fi

# Test 1: a flow matched by the rule gets the policy timeout.
conn_pid=$(open_connection "$PORT_POLICY")
check_timeout "$PORT_POLICY" policy test_policy_applied
close_connection "$conn_pid"

# Test 2: a flow not matched by any rule keeps the kernel default.
conn_pid=$(open_connection "$PORT_DEFAULT")
check_timeout "$PORT_DEFAULT" default test_default_preserved
close_connection "$conn_pid"

# Test 3: the policy survives a delete/reload cycle of the whole table.
ip netns exec "$ns1" conntrack -F 2>/dev/null
ip netns exec "$ns1" nft delete table ip ct_timeout_test

if ! load_ruleset; then
	echo "FAIL: test_policy_reload: could not reload ct timeout ruleset"
	exit $ksft_fail
fi

conn_pid=$(open_connection "$PORT_POLICY")
check_timeout "$PORT_POLICY" policy test_policy_reload
close_connection "$conn_pid"

exit $ret
