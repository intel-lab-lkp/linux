#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Stress nftables ct timeout object destruction while new TCP flows keep
# attaching the object.

net_netfilter_dir=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")
source "$net_netfilter_dir/lib.sh"

checktool "nft --version" "run test without nft tool"

read kernel_tainted < /proc/sys/kernel/tainted

# Default to 80% of the global timeout but keep this stress test short.
TEST_RUNTIME=$((${kselftest_timeout:-30} * 8 / 10))
[[ $TEST_RUNTIME -gt 20 ]] && TEST_RUNTIME=20

PORT=12345

cleanup()
{
	cleanup_all_ns
}

load_ruleset()
{
	ip netns exec "$ns1" nft -f - <<EOF
table ip ct_test {
	ct timeout tcptime {
		protocol tcp
		policy = { established: 5s }
	}

	chain output {
		type filter hook output priority filter; policy accept;
		ct state new ip daddr 10.0.1.2 tcp dport $PORT counter ct timeout set "tcptime"
	}
}
EOF
}

flush_table()
{
	ip netns exec "$ns1" nft flush table ip ct_test 2>/dev/null || true
	ip netns exec "$ns1" nft delete table ip ct_test 2>/dev/null || true
}

rule_packets()
{
	local packets

	packets=$(ip netns exec "$ns1" nft list chain ip ct_test output 2>/dev/null |
		sed -n 's/.*counter packets \([0-9][0-9]*\) bytes.*/\1/p' |
		head -n1)

	if [ -n "$packets" ]; then
		echo "$packets"
	else
		echo 0
	fi
}

trap cleanup EXIT

setup_ns ns1 ns2 || exit $ksft_skip

if ! ip link add veth0 netns "$ns1" type veth peer name veth0 netns "$ns2" > /dev/null 2>&1; then
	echo "SKIP: No virtual ethernet pair device support in kernel"
	exit $ksft_skip
fi

ip -net "$ns1" link set veth0 up
ip -net "$ns2" link set veth0 up

ip -net "$ns1" addr add 10.0.1.1/24 dev veth0
ip -net "$ns2" addr add 10.0.1.2/24 dev veth0

if ! load_ruleset; then
	echo "SKIP: Could not load ct timeout ruleset"
	exit $ksft_skip
fi

ip netns exec "$ns1" bash -c '
	while :; do
		exec 3<>/dev/tcp/10.0.1.2/'"$PORT"' 2>/dev/null || true
		exec 3<&- 3>&-
	done
' > /dev/null 2>&1 &
traffic_pid=$!

if ! busywait_for_counter "$BUSYWAIT_TIMEOUT" 1 rule_packets > /dev/null; then
	echo "FAIL: Did not observe TCP traffic hitting ct timeout rule"
	exit $ksft_fail
fi

end_time=$((SECONDS + TEST_RUNTIME))
while [ "$SECONDS" -lt "$end_time" ]; do
	flush_table

	if ! load_ruleset; then
		echo "FAIL: Could not recreate ct timeout ruleset"
		exit $ksft_fail
	fi
done

flush_table

kill "$traffic_pid" 2>/dev/null
wait "$traffic_pid" 2>/dev/null

if [[ $kernel_tainted -eq 0 && $(</proc/sys/kernel/tainted) -ne 0 ]]; then
	echo "FAIL: Kernel is tainted"
	exit $ksft_fail
fi

exit $ksft_pass
