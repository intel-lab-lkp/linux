#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Check that routes with a lightweight tunnel encap survive "ip route save"
# followed by "ip route restore", which sends the dumped RTA_ENCAP back to
# the kernel unchanged.

# shellcheck disable=SC1091,SC2034,SC2154,SC2317
source lib.sh

ALL_TESTS="
	save_restore_rpl
	save_restore_ioam6
	save_restore_xfrm
	save_restore_geneve_opts
	save_restore_vxlan_opts
	save_restore_erspan_opts
"

setup_prepare()
{
	setup_ns NS
	defer cleanup_all_ns

	ip -n "$NS" link add name dummy0 up type dummy
}

# save_restore <description> <prefix> <ip route add arguments>
#
# The description starts with the encap type.
save_restore()
{
	local desc=$1; shift
	local prefix=$1
	local encap=${desc%% *}
	local before after dump out rc
	local family=-4

	[[ $prefix == *:* ]] && family=-6

	RET=0

	if ! ip route help 2>&1 | grep "^ENCAPTYPE" | grep -qw "$encap"; then
		log_test_skip "$desc" "iproute2 lacks the encap"
		return
	fi

	out=$(ip -n "$NS" "$family" route add "$@" 2>&1)
	rc=$?
	if ((rc)) && [[ $out == *"encapsulation type"* ||
			$out == *CONFIG_LWTUNNEL* ]]; then
		log_test_skip "$desc" "kernel lacks the encap"
		return
	fi
	check_err "$rc" "Failed to add route: $out"

	dump=$(mktemp)
	defer rm -f "$dump"

	before=$(ip -n "$NS" "$family" route show "$prefix")
	ip -n "$NS" "$family" route save "$prefix" > "$dump"
	check_err $? "Failed to save route"

	ip -n "$NS" "$family" route del "$prefix"
	ip -n "$NS" "$family" route restore < "$dump"
	check_err $? "Failed to restore route"

	after=$(ip -n "$NS" "$family" route show "$prefix")
	[ "$before" = "$after" ]
	check_err $? "Restored route differs from the saved one"

	log_test "encap $desc route save and restore"
}

save_restore_rpl()
{
	save_restore rpl 2001:db8:1::/64 \
		encap rpl segs 2001:db8::2 dev dummy0
}

save_restore_ioam6()
{
	save_restore ioam6 2001:db8:2::/64 \
		encap ioam6 trace prealloc type 0x800000 ns 1 size 12 dev dummy0
}

save_restore_xfrm()
{
	# iproute2 takes every argument after "encap xfrm" as an xfrm one
	save_restore xfrm 2001:db8:3::/64 dev dummy0 encap xfrm if_id 1
}

save_restore_geneve_opts()
{
	save_restore "ip geneve_opts" 192.0.2.0/26 \
		encap ip id 1 dst 198.51.100.2 geneve_opts 0:0:12121212 \
		dev dummy0
}

save_restore_vxlan_opts()
{
	save_restore "ip vxlan_opts" 192.0.2.64/26 \
		encap ip id 1 dst 198.51.100.2 vxlan_opts 456 dev dummy0
}

save_restore_erspan_opts()
{
	save_restore "ip erspan_opts" 192.0.2.128/26 \
		encap ip id 1 dst 198.51.100.2 erspan_opts 1:123:0:0 dev dummy0
}

trap defer_scopes_cleanup EXIT
setup_prepare
tests_run

exit "$EXIT_STATUS"
