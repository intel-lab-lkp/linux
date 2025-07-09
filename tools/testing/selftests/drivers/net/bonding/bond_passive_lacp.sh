#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Testing if bond works with lacp_active = off

lib_dir=$(dirname "$0")
source ${lib_dir}/bond_topo_lacp.sh

trap cleanup EXIT
setup_topo_lacp

lacp_bond_reset "${c_ns}" "lacp_active off"
# make sure the switch state is not expired [A,T,G,S,Ex]
if slowwait 15 ip netns exec ${s_ns} grep -q 'port state: 143' /proc/net/bonding/bond0; then
	RET=1
else
	RET=0
fi
log_test "bond 802.3ad" "lacp_active off"

exit $EXIT_STATUS
