#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# A test script for nf_max_table_jumps_netns limit sysctl
#
source lib.sh

DEFAULT_SYSCTL=65536

user_owned_netns="a_user_owned_netns"

cleanup() {
        ip netns del $user_owned_netns 2>/dev/null || true
}

trap cleanup EXIT

init_net_value=$(sysctl -n net.netfilter.nf_max_table_jumps_netns)

# Check that init ns inits to default value
if [ "$init_net_value" -ne "$DEFAULT_SYSCTL" ];then
	echo "Fail: Does not init default value"
	exit 1
fi

# Set to extremely small, demonstrate CAN exceed value
sysctl -w net.netfilter.nf_max_table_jumps_netns=32 2>&1 >/dev/null
new_value=$(sysctl -n net.netfilter.nf_max_table_jumps_netns)
if [ "$new_value" -ne "32" ];then
	echo "Fail: Set value not respected"
	exit 1
fi

err_string=$(
nft -f - <<EOF
table inet loop-test {
	chain test0 {
		type filter hook input priority filter; policy accept;
		jump test1
		jump test1
	}

	chain test1 {
		jump test2
		jump test2
	}

	chain test2 {
		jump test3
		tcp dport 8080 drop
		tcp dport 8080 drop
	}

	chain test3 {
		jump test4
	}

	chain test4 {
		jump test5
	}

	chain test5 {
		jump test6
	}

	chain test6 {
		jump test7
	}

	chain test7 {
		jump test8
	}

	chain test8 {
		jump test9
	}

	chain test9 {
		jump test10
	}

	chain test10 {
		jump test11
	}

	chain test11 {
		jump test12
	}

	chain test12 {
		jump test13
	}

	chain test13 {
		jump test14
	}

	chain test14 {
		jump test15
		jump test15
	}

	chain test15 {
	}
}
EOF

)
if [[ "$err_string" != "" ]];then
	echo "Fail: limit not exceeded when expected"
	exit 1
fi

nft flush ruleset

# reset to default
sysctl -w net.netfilter.nf_max_table_jumps_netns=$DEFAULT_SYSCTL 2>&1 >/dev/null

# Make init_user_ns owned netns, can change value, limit is applied
ip netns add $user_owned_netns
err_string=$(ip netns exec $user_owned_netns sysctl -qw net.netfilter.nf_max_table_jumps_netns=32 2>&1)
if [[ "$err_string" != "" ]];then
	echo "Fail: Can't change value in init_user_ns owned namespace"
	exit 1
fi
err_string=$(
ip netns exec $user_owned_netns \
nft -f - 2>&1 <<EOF
table inet loop-test {
	chain test0 {
		type filter hook input priority filter; policy accept;
		jump test1
		jump test1
	}

	chain test1 {
		jump test2
		jump test2
	}

	chain test2 {
		jump test3
		tcp dport 8080 drop
		tcp dport 8080 drop
	}

	chain test3 {
		jump test4
	}

	chain test4 {
		jump test5
	}

	chain test5 {
		jump test6
	}

	chain test6 {
		jump test7
	}

	chain test7 {
		jump test8
	}

	chain test8 {
		jump test9
	}

	chain test9 {
		jump test10
	}

	chain test10 {
		jump test11
	}

	chain test11 {
		jump test12
	}

	chain test12 {
		jump test13
	}

	chain test13 {
		jump test14
	}

	chain test14 {
		jump test15
		jump test15
	}

	chain test15 {
	}
}
EOF
)
if [[ "$err_string" != *"Too many links"* ]];then
	echo "Fail: Limited incorrectly applied"
	exit 1
fi
ip netns del $user_owned_netns

# Previously set value does not impact root namespace; check value from before
new_value=$(sysctl -n net.netfilter.nf_max_table_jumps_netns)
if [ "$new_value" -ne "$DEFAULT_SYSCTL" ];then
	echo "Fail: Non-init namespace altered init namespace"
	exit 1
fi

# Make non-init_user_ns owned netns, can not change value
err_string=$(unshare -Un sysctl -w net.netfilter.nf_max_table_jumps_netns=1234 2>&1)
if [[ "$err_string" != *"Operation not permitted"* ]];then
	echo "Fail: Error message incorrect when non-user-init"
	exit 1
fi

exit 0
