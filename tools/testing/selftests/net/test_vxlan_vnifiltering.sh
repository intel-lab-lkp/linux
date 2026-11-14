#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This test is for checking the VXLAN vni filtering api and
# datapath.
# It simulates two hypervisors running two VMs each using four network
# six namespaces: two for the HVs, four for the VMs. Each VM is
# connected to a separate bridge. The VM's use overlapping vlans and
# hence the separate bridge domain. Each vxlan device is a collect
# metadata device with vni filtering and hence has the ability to
# terminate configured vni's only.

#  +--------------------------------+     +------------------------------------+
#  |  vm-11 netns                   |     |  vm-21 netns                       |
#  |                                |     |                                    |
#  |+------------+  +-------------+ |     |+-------------+ +----------------+  |
#  ||veth-11.10  |  |veth-11.20   | |     ||veth-21.10   | | veth-21.20     |  |
#  ||10.0.10.11/24  |10.0.20.11/24| |     ||10.0.10.21/24| | 10.0.20.21/24  |  |
#  |+------|-----+  +|------------+ |     |+-----------|-+ +---|------------+  |
#  |       |         |              |     |            |       |               |
#  |       |         |              |     |         +------------+             |
#  |      +------------+            |     |         | veth-21    |             |
#  |      | veth-11    |            |     |         |            |             |
#  |      |            |            |     |         +-----|------+             |
#  |      +-----|------+            |     |               |                    |
#  |            |                   |     |               |                    |
#  +------------|-------------------+     +---------------|--------------------+
#  +------------|-----------------------------------------|-------------------+
#  |      +-----|------+                            +-----|------+            |
#  |      |vethhv-11   |                            |vethhv-21   |            |
#  |      +----|-------+                            +-----|------+            |
#  |       +---|---+                                  +---|--+                |
#  |       |  br1  |                                  | br2  |                |
#  |       +---|---+                                  +---|--+                |
#  |       +---|----+                                 +---|--+                |
#  |       |  vxlan1|                                 |vxlan2|                |
#  |       +--|-----+                                 +--|---+                |
#  |          |                                          |                    |
#  |          |         +---------------------+          |                    |
#  |          |         |veth0                |          |                    |
#  |          +---------|172.16.0.1/24        -----------+                    |
#  |                    |2002:fee1::1/64      |                               |
#  | hv-1 netns         +--------|------------+                               |
#  +-----------------------------|--------------------------------------------+
#                                |
#  +-----------------------------|--------------------------------------------+
#  | hv-2 netns         +--------|-------------+                              |
#  |                    | veth0                |                              |
#  |             +------| 172.16.0.2/24        |---+                          |
#  |             |      | 2002:fee1::2/64      |   |                          |
#  |             |      |                      |   |                          |
#  |             |      +----------------------+   |         -                |
#  |             |                                 |                          |
#  |           +-|-------+                +--------|-+                        |
#  |           | vxlan1  |                |  vxlan2  |                        |
#  |           +----|----+                +---|------+                        |
#  |             +--|--+                    +-|---+                           |
#  |             | br1 |                    | br2 |                           |
#  |             +--|--+                    +--|--+                           |
#  |          +-----|-------+             +----|-------+                      |
#  |          | vethhv-12   |             |vethhv-22   |                      |
#  |          +------|------+             +-------|----+                      |
#  +-----------------|----------------------------|---------------------------+
#                    |                            |
#  +-----------------|-----------------+ +--------|---------------------------+
#  |         +-------|---+             | |     +--|---------+                 |
#  |         | veth-12   |             | |     |veth-22     |                 |
#  |         +-|--------|+             | |     +--|--------|+                 |
#  |           |        |              | |        |        |                  |
#  |+----------|--+ +---|-----------+  | |+-------|-----+ +|---------------+  |
#  ||veth-12.10   | |veth-12.20     |  | ||veth-22.10   | |veth-22.20      |  |
#  ||10.0.10.12/24| |10.0.20.12/24  |  | ||10.0.10.22/24| |10.0.20.22/24   |  |
#  |+-------------+ +---------------+  | |+-------------+ +----------------+  |
#  |                                   | |                                    |
#  |                                   | |                                    |
#  | vm-12 netns                       | |vm-22 netns                         |
#  +-----------------------------------+ +------------------------------------+
#
#
# This test tests the new vxlan vnifiltering api
source lib.sh
ret=0

# all tests in this script. Can be overridden with -t option
TESTS="
	vxlan_vnifilter_api
	vxlan_vnifilter_dump_replay
	vxlan_vnifilter_add_rollback
	vxlan_vnifilter_refused_fdb_update
	vxlan_vnifilter_datapath
	vxlan_vnifilter_datapath_pervni
	vxlan_vnifilter_datapath_mgroup
	vxlan_vnifilter_datapath_mgroup_pervni
	vxlan_vnifilter_metadata_and_traditional_mix
"
VERBOSE=0
PAUSE_ON_FAIL=no
PAUSE=no

which ping6 > /dev/null 2>&1 && ping6=$(which ping6) || ping6=$(which ping)

log_test()
{
	log_test_expected "$1" "$2" "$3"
}

run_cmd()
{
	local cmd="$1"
	local out
	local stderr="2>/dev/null"

	if [ "$VERBOSE" = "1" ]; then
		printf "COMMAND: $cmd\n"
		stderr=
	fi

	out=$(eval $cmd $stderr)
	rc=$?
	if [ "$VERBOSE" = "1" -a -n "$out" ]; then
		echo "    $out"
	fi

	return $rc
}

check_hv_connectivity() {
	slowwait 5 ip netns exec $hv_1 ping -c 1 -W 1 $1 &>/dev/null
	slowwait 5 ip netns exec $hv_1 ping -c 1 -W 1 $2 &>/dev/null

	return $?
}

check_vm_connectivity() {
	slowwait 5 run_cmd "ip netns exec $vm_11 ping -c 1 -W 1 10.0.10.12"
	log_test $? 0 "VM connectivity over $1 (ipv4 default rdst)"

	slowwait 5 run_cmd "ip netns exec $vm_21 ping -c 1 -W 1 10.0.10.22"
	log_test $? 0 "VM connectivity over $1 (ipv6 default rdst)"
}

cleanup() {
	ip link del veth-hv-1 2>/dev/null || true
	ip link del vethhv-11 vethhv-12 vethhv-21 vethhv-22 2>/dev/null || true
	ip link del veth-host 2>/dev/null || true

	cleanup_ns $hv_1 $hv_2 $vm_11 $vm_21 $vm_12 $vm_22 $vm_31 $vm_32 $testns
}

trap cleanup EXIT

setup-hv-networking() {
	id=$1
	local1=$2
	mask1=$3
	local2=$4
	mask2=$5

	ip link set veth-hv-$id netns ${hv[$id]}
	ip -netns ${hv[$id]} link set veth-hv-$id name veth0
	ip -netns ${hv[$id]} addr add $local1/$mask1 dev veth0
	ip -netns ${hv[$id]} addr add $local2/$mask2 dev veth0
	ip -netns ${hv[$id]} link set veth0 up
}

# Setups a "VM" simulated by a netns an a veth pair
# example: setup-vm <hvid> <vmid> <brid> <VATTRS> <mcast_for_bum>
# VATTRS = comma separated "<vlan>-<v[46]>-<localip>-<remoteip>-<VTYPE>-<vxlandstport>"
# VTYPE = vxlan device type. "default = traditional device, metadata = metadata device
#         vnifilter = vnifiltering device,
#         vnifilterg = vnifiltering device with per vni group/remote"
# example:
#     setup-vm 1 11 1 \
#         10-v4-172.16.0.1-239.1.1.100-vnifilterg,20-v4-172.16.0.1-239.1.1.100-vnifilterg 1
#
setup-vm() {
	hvid=$1
	vmid=$2
	brid=$3
	vattrs=$4
	mcast=$5
	lastvxlandev=""

	# create bridge
	ip -netns ${hv[$hvid]} link add br$brid type bridge vlan_filtering 1 vlan_default_pvid 0 \
		mcast_snooping 0
	ip -netns ${hv[$hvid]} link set br$brid up

	# create vm namespace and interfaces and connect to hypervisor
	# namespace
	hvvethif="vethhv-$vmid"
	vmvethif="veth-$vmid"
	ip link add $hvvethif type veth peer name $vmvethif
	ip link set $hvvethif netns ${hv[$hvid]}
	ip link set $vmvethif netns ${vm[$vmid]}
	ip -netns ${hv[$hvid]} link set $hvvethif up
	ip -netns ${vm[$vmid]} link set $vmvethif up
	ip -netns ${hv[$hvid]} link set $hvvethif master br$brid

	# configure VM vlan/vni filtering on hypervisor
	for vmap in $(echo $vattrs | cut -d "," -f1- --output-delimiter=' ')
	do
	local vid=$(echo $vmap | awk -F'-' '{print ($1)}')
	local family=$(echo $vmap | awk -F'-' '{print ($2)}')
	local localip=$(echo $vmap | awk -F'-' '{print ($3)}')
	local group=$(echo $vmap | awk -F'-' '{print ($4)}')
	local vtype=$(echo $vmap | awk -F'-' '{print ($5)}')
	local port=$(echo $vmap | awk -F'-' '{print ($6)}')

	ip -netns ${vm[$vmid]} link add name $vmvethif.$vid link $vmvethif type vlan id $vid
	ip -netns ${vm[$vmid]} addr add 10.0.$vid.$vmid/24 dev $vmvethif.$vid
	ip -netns ${vm[$vmid]} link set $vmvethif.$vid up

	tid=$vid
	vxlandev="vxlan$brid"
	vxlandevflags=""

	if [[ -n $vtype && $vtype == "metadata" ]]; then
	   vxlandevflags="$vxlandevflags external"
	elif [[ -n $vtype && $vtype == "vnifilter" || $vtype == "vnifilterg" ]]; then
	   vxlandevflags="$vxlandevflags external vnifilter"
	   tid=$((vid+brid))
	else
	   vxlandevflags="$vxlandevflags id $tid"
	   vxlandev="vxlan$tid"
	fi

	if [[ -n $vtype && $vtype != "vnifilterg" ]]; then
	   if [[ -n "$group" && "$group" != "null" ]]; then
	      if [ $mcast -eq 1 ]; then
		 vxlandevflags="$vxlandevflags group $group"
	      else
		 vxlandevflags="$vxlandevflags remote $group"
	      fi
	   fi
	fi

	if [[ -n "$port" && "$port" != "default" ]]; then
	      vxlandevflags="$vxlandevflags dstport $port"
	fi

	# create vxlan device
	if [ "$vxlandev" != "$lastvxlandev" ]; then
	     ip -netns ${hv[$hvid]} link add $vxlandev type vxlan local $localip $vxlandevflags dev veth0 2>/dev/null
	     ip -netns ${hv[$hvid]} link set $vxlandev master br$brid
	     ip -netns ${hv[$hvid]} link set $vxlandev up
	     lastvxlandev=$vxlandev
	fi

	# add vlan
	bridge -netns ${hv[$hvid]} vlan add vid $vid dev $hvvethif
	bridge -netns ${hv[$hvid]} vlan add vid $vid pvid dev $vxlandev

	# Add bridge vni filter for tx
	if [[ -n $vtype && $vtype == "metadata" || $vtype == "vnifilter" || $vtype == "vnifilterg" ]]; then
	   bridge -netns ${hv[$hvid]} link set dev $vxlandev vlan_tunnel on
	   bridge -netns ${hv[$hvid]} vlan add dev $vxlandev vid $vid tunnel_info id $tid
	fi

	if [[ -n $vtype && $vtype == "metadata" ]]; then
	   bridge -netns ${hv[$hvid]} fdb add 00:00:00:00:00:00 dev $vxlandev \
								src_vni $tid vni $tid dst $group self
	elif [[ -n $vtype && $vtype == "vnifilter" ]]; then
	   # Add per vni rx filter with 'bridge vni' api
	   bridge -netns ${hv[$hvid]} vni add dev $vxlandev vni $tid
	elif [[ -n $vtype && $vtype == "vnifilterg" ]]; then
	   # Add per vni group config with 'bridge vni' api
	   if [ -n "$group" ]; then
		if [ $mcast -eq 1 ]; then
			bridge -netns ${hv[$hvid]} vni add dev $vxlandev vni $tid group $group
		else
			bridge -netns ${hv[$hvid]} vni add dev $vxlandev vni $tid remote $group
		fi
	   fi
	fi
	done
}

setup_vnifilter_api()
{
	ip link add veth-host type veth peer name veth-testns
	setup_ns testns
	ip link set veth-testns netns $testns
}

cleanup_vnifilter_api()
{
	ip link del veth-host 2>/dev/null || true
	ip netns del $testns 2>/dev/null || true
}

# Succeed if "bridge vni show" lists VNI $2 on device $1. A failed show
# returns 2, so a probe expecting the VNI to be absent cannot pass on it.
vni_listed()
{
	local out

	out=$(bridge -netns $testns vni show dev $1) || return 2
	grep -qw "$2" <<< "$out"
}

# Print the VNI ranges "bridge vni show" reports for device $1, sorted;
# fail if the show itself fails.
vni_ranges()
{
	local out

	out=$(bridge -netns $testns vni show dev $1) || return 1
	grep -oE '[0-9]+-[0-9]+|[0-9]{2,}' <<< "$out" | sort
}

# Succeed if "bridge fdb show" lists, on device $1, an entry whose line
# matches the extended regex $2. A failed show returns 2, so a probe
# expecting the entry to be absent cannot pass on it.
fdb_listed()
{
	local out

	out=$(bridge -netns $testns fdb show dev $1) || return 2
	grep -qE "$2" <<< "$out"
}

# tests vxlan filtering api
vxlan_vnifilter_api()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"
	localip="172.16.0.1"
	group="239.1.1.101"

	cleanup_vnifilter_api &>/dev/null
	setup_vnifilter_api

	# Duplicate vni test
	# create non-vnifiltering traditional vni device
	run_cmd "ip -netns $testns link add vxlan100 type vxlan id 100 local $localip dev veth-testns dstport 4789"
	log_test $? 0 "Create traditional vxlan device"

	# create vni filtering device
	run_cmd "ip -netns $testns link add vxlan-ext1 type vxlan vnifilter local $localip dev veth-testns dstport 4789"
	log_test $? 1 "Cannot create vnifilter device without external flag"

	run_cmd "ip -netns $testns link add vxlan-ext1 type vxlan external vnifilter local $localip dev veth-testns dstport 4789"
	log_test $? 0 "Creating external vxlan device with vnifilter flag"

	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 100"
	log_test $? 0 "Cannot set in-use vni id on vnifiltering device"

	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 200"
	log_test $? 0 "Set new vni id on vnifiltering device"

	run_cmd "ip -netns $testns link add vxlan-ext2 type vxlan external vnifilter local $localip dev veth-testns dstport 4789"
	log_test $? 0 "Create second external vxlan device with vnifilter flag"

	run_cmd "bridge -netns $testns vni add dev vxlan-ext2 vni 200"
	log_test $? 255 "Cannot set in-use vni id on vnifiltering device"

	run_cmd "bridge -netns $testns vni add dev vxlan-ext2 vni 300"
	log_test $? 0 "Set new vni id on vnifiltering device"

	# check in bridge vni show
	run_cmd "bridge -netns $testns vni add dev vxlan-ext2 vni 300"
	log_test $? 0 "Update vni id on vnifiltering device"

	run_cmd "bridge -netns $testns vni add dev vxlan-ext2 vni 400"
	log_test $? 0 "Add new vni id on vnifiltering device"

	# add multicast group per vni
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 200 group $group"
	log_test $? 0 "Set multicast group on existing vni"

	# add multicast group per vni
	run_cmd "bridge -netns $testns vni add dev vxlan-ext2 vni 300 group $group"
	log_test $? 0 "Set multicast group on existing vni"

	# set vnifilter on an existing external vxlan device
	run_cmd "ip -netns $testns link set dev vxlan-ext1 type vxlan external vnifilter"
	log_test $? 2 "Cannot set vnifilter flag on a device"

	# change vxlan vnifilter flag
	run_cmd "ip -netns $testns link set dev vxlan-ext1 type vxlan external novnifilter"
	log_test $? 2 "Cannot unset vnifilter flag on a device"

	# A single request may add at most 4096 VNIs. bridge(8) puts one
	# VXLAN_VNIFILTER_ENTRY per comma-separated item into a single message,
	# so both the single-entry and the multi-entry paths are reachable here.

	# The largest accepted add, and one VNI more rejected.
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 10000-14095"
	log_test $? 0 "Add a request of the maximum VNI count"

	# The rejected oversized add must install nothing. Assert the range is
	# absent first: vxlan_vni_add() folds an already-present VNI into the
	# update path and returns 0, so a later probe cannot tell "installed
	# nothing" from "installed part" unless it started absent.
	run_cmd "vni_listed vxlan-ext1 20000"
	log_test $? 1 "VNI 20000 absent before the oversized add"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 20000-24096"
	log_test $? 255 "Cannot add a request over the maximum VNI count"
	run_cmd "vni_listed vxlan-ext1 20000"
	log_test $? 1 "The rejected oversized add installed nothing"

	# Two entries each under the limit but summing over it: the per-message
	# total is what is bounded, not the span of one entry. This is the shape
	# a per-entry check would have let through.
	run_cmd "vni_listed vxlan-ext1 30000"
	log_test $? 1 "VNI 30000 absent before the oversized multi-entry add"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 \
		 vni 30000-32047,32048-34097"
	log_test $? 255 "Cannot add multiple entries summing over the maximum"
	run_cmd "vni_listed vxlan-ext1 30000"
	log_test $? 1 "The rejected multi-entry add installed nothing"

	# The cap is symmetric: a delete may touch at most the maximum too.
	# Install a maximum-size range and the VNI past it, so the max+1 delete
	# below has every VNI present and can only be refused by the cap, not by
	# a missing VNI.
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 40000-44095"
	log_test $? 0 "Populate a maximum-size range to delete"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 44096"
	log_test $? 0 "Add the VNI past the maximum range"
	run_cmd "bridge -netns $testns vni del dev vxlan-ext1 vni 40000-44096"
	log_test $? 255 "Cannot delete a request over the maximum VNI count"
	run_cmd "bridge -netns $testns vni del dev vxlan-ext1 vni 40000-44095"
	log_test $? 0 "Delete a request of the maximum VNI count"
	run_cmd "bridge -netns $testns vni del dev vxlan-ext1 vni 44096"
	log_test $? 0 "Delete the VNI past the maximum range"

	# A within-cap delete of a never-installed range reaches the handler and
	# fails there on the missing VNI, not on the cap.
	run_cmd "bridge -netns $testns vni del dev vxlan-ext1 vni 50000-50010"
	log_test $? 255 "Cannot delete a range that was never installed"

	# A start above the end selects nothing and is accepted as a no-op.
	# Use a VNI no earlier case installs, so the absence check is
	# meaningful.
	run_cmd "vni_listed vxlan-ext1 55000"
	log_test $? 1 "VNI 55000 absent before the inverted range"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 55000-50000"
	log_test $? 0 "An inverted range is accepted as a no-op"
	run_cmd "vni_listed vxlan-ext1 55000"
	log_test $? 1 "The inverted range installed nothing"

	# The VXLAN header carries 24 bits, and both endpoints are bounded. A
	# lone VNI is sent as START only and spans one VNI, so only the START
	# bound refuses it; a range from a valid START spans two VNIs, so only
	# the END bound refuses it. Neither is large enough for the request
	# limit to catch.
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 16777215"
	log_test $? 0 "Add the highest VNI the header can carry"
	run_cmd "bridge -netns $testns vni del dev vxlan-ext1 vni 16777215"
	log_test $? 0 "Delete the highest VNI the header can carry"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 vni 16777216"
	log_test $? 255 "Cannot add a VNI past the 24-bit space"
	run_cmd "bridge -netns $testns vni add dev vxlan-ext1 \
		 vni 16777215-16777216"
	log_test $? 255 "Cannot add a range whose END leaves the 24-bit space"
}

# A device may hold a contiguous run longer than one request's limit, built
# from several requests. The dump coalesces it, so the dump must break the run
# into entries each no larger than the limit, or the configuration it reports
# cannot be replayed. Install such a run, then feed every range the dump
# reports back into a second device and require all to be accepted.
vxlan_vnifilter_dump_replay()
{
	local opts="external vnifilter local 172.16.0.1 dev veth-testns"
	local range replay_rc src_ranges dst_ranges

	cleanup_vnifilter_api &>/dev/null
	setup_vnifilter_api

	# The destination is on a different dstport so re-adding the same VNIs
	# does not collide with the source under vxlan_vni_in_use().
	run_cmd "ip -netns $testns link add vxlan-src type vxlan $opts \
		 dstport 4789"
	log_test $? 0 "dump/replay: create source device"
	run_cmd "ip -netns $testns link add vxlan-dst type vxlan $opts \
		 dstport 4790"
	log_test $? 0 "dump/replay: create destination device"

	# 8192 contiguous VNIs sharing the default remote: one run, twice the
	# limit, installed in two accepted requests.
	run_cmd "bridge -netns $testns vni add dev vxlan-src vni 10000-14095"
	log_test $? 0 "dump/replay: populate the first half of the run"
	run_cmd "bridge -netns $testns vni add dev vxlan-src vni 14096-18191"
	log_test $? 0 "dump/replay: populate the second half of the run"

	# The run must come back split at the limit, exactly. Comparing with
	# the literal split, not only with the replayed copy, also catches a
	# dump that drops or repeats VNIs where it resumes.
	src_ranges=$(vni_ranges vxlan-src)
	log_test $? 0 "dump/replay: dump the source device"
	[ "$src_ranges" = "$(printf '10000-14095\n14096-18191')" ]
	log_test $? 0 "dump/replay: the run is dumped split at the limit"

	# bridge(8) prints entries, not netlink messages, so this replays one
	# entry per request; it does not see the per-message budget.
	replay_rc=0
	for range in $src_ranges; do
		bridge -netns $testns vni add dev vxlan-dst vni "$range" \
			2>/dev/null || replay_rc=$?
	done
	log_test $replay_rc 0 "dump/replay: replay accepts every dumped entry"

	dst_ranges=$(vni_ranges vxlan-dst)
	log_test $? 0 "dump/replay: dump the destination device"
	[ "$src_ranges" = "$dst_ranges" ]
	log_test $? 0 "dump/replay: source and destination dump the same ranges"
}

# A VNI whose add fails must not be left installed. Two steps can fail
# after the VNI is published, and both are reachable without memory
# pressure: installing its default FDB entry, once the device holds
# maxaddress entries, and joining its group, once the socket holds
# net.ipv4.igmp_max_memberships groups.
vxlan_vnifilter_add_rollback()
{
	local opts="external vnifilter local 172.16.0.1 dev veth-testns"
	local full="dstport 4789 remote 172.16.0.2 maxaddress 1"
	local groups grp vni k fail=0

	cleanup_vnifilter_api &>/dev/null
	setup_vnifilter_api

	# The device's own default entry uses up maxaddress 1, so no VNI that
	# inherits the device remote can install its own.
	run_cmd "ip -netns $testns link add vxlan-full type vxlan $opts $full"
	log_test $? 0 "rollback: create a device with room for one FDB entry"
	run_cmd "ip -netns $testns link set dev vxlan-full up"
	run_cmd "bridge -netns $testns vni add dev vxlan-full vni 100"
	log_test $? 255 "rollback: refuse a VNI whose FDB entry does not fit"
	run_cmd "vni_listed vxlan-full 100"
	log_test $? 1 "rollback: the VNI whose FDB entry failed is absent"

	groups=$(ip netns exec $testns sysctl -n net.ipv4.igmp_max_memberships)
	run_cmd "ip -netns $testns link add vxlan-mc type vxlan $opts \
		 dstport 4790"
	log_test $? 0 "rollback: create a device for multicast groups"
	run_cmd "ip -netns $testns link set dev vxlan-mc up"
	# VNIs above 255 so that no group address can match the VNI probe.
	for k in $(seq 1 $groups); do
		grp=239.1.$((k / 256)).$((k % 256))
		bridge -netns $testns vni add dev vxlan-mc vni $((1000 + k)) \
			group $grp 2>/dev/null || fail=1
	done
	log_test $fail 0 "rollback: join as many groups as the socket allows"
	vni=$((1001 + groups))
	run_cmd "bridge -netns $testns vni add dev vxlan-mc vni $vni \
		 group 239.2.0.1"
	log_test $? 255 "rollback: refuse a VNI whose group cannot be joined"
	run_cmd "vni_listed vxlan-mc $vni"
	log_test $? 1 "rollback: the VNI whose join failed is not installed"
}

# A refused FDB update must leave the entry's state as it was. Replacing a
# VNI's permanent default (all-zeros) entry with a dynamic one is refused,
# because only unicast entries can be replaced, so the entry must stay
# permanent and survive ageing. A dynamic entry added after the refused
# request is due for ageing no earlier than the default entry, so once it
# has been aged out the default entry has been through the same pass. The
# check is on the end state only: a request that changed the state and
# put it back before returning would pass it too.
vxlan_vnifilter_refused_fdb_update()
{
	local opts="external vnifilter local 172.16.0.1 dev veth-testns"
	local zero="^00:00:00:00:00:00 dst 172.16.0.2 .*src_vni 100 .*self"
	local mac=02:00:00:00:00:01

	cleanup_vnifilter_api &>/dev/null
	setup_vnifilter_api

	run_cmd "ip -netns $testns link add vxlan-age type vxlan $opts \
		 dstport 4789 ageing 1"
	log_test $? 0 "refused update: create a device with ageing 1"
	run_cmd "ip -netns $testns link set dev vxlan-age up"
	run_cmd "bridge -netns $testns vni add dev vxlan-age vni 100 \
		 remote 172.16.0.2"
	log_test $? 0 "refused update: add a VNI with a remote"
	run_cmd "fdb_listed vxlan-age '$zero permanent'"
	log_test $? 0 "refused update: the VNI's default entry is permanent"
	run_cmd "bridge -netns $testns fdb replace 00:00:00:00:00:00 \
		 dev vxlan-age dst 172.16.0.3 src_vni 100 self dynamic"
	log_test $? 255 "refused update: cannot replace the default entry"
	run_cmd "bridge -netns $testns fdb add $mac dev vxlan-age \
		 dst 172.16.0.4 src_vni 100 self dynamic"
	log_test $? 0 "refused update: add a dynamic entry"
	slowwait 25 eval "! fdb_listed vxlan-age '^$mac '"
	log_test $? 0 "refused update: the dynamic entry is aged out"
	run_cmd "fdb_listed vxlan-age '$zero permanent'"
	log_test $? 0 "refused update: the default entry is still permanent"
}

# Sanity test vnifilter datapath
# vnifilter vnis inherit BUM group from
# vxlan device
vxlan_vnifilter_datapath()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"

	setup_ns hv_1 hv_2
	hv[1]=$hv_1
	hv[2]=$hv_2
	ip link add veth-hv-1 type veth peer name veth-hv-2
	setup-hv-networking 1 $hv1addr1 24 $hv1addr2 64 $hv2addr1 $hv2addr2
	setup-hv-networking 2 $hv2addr1 24 $hv2addr2 64 $hv1addr1 $hv1addr2

        check_hv_connectivity hv2addr1 hv2addr2

	setup_ns vm_11 vm_21 vm_12 vm_22
	vm[11]=$vm_11
	vm[21]=$vm_21
	vm[12]=$vm_12
	vm[22]=$vm_22
	setup-vm 1 11 1 10-v4-$hv1addr1-$hv2addr1-vnifilter,20-v4-$hv1addr1-$hv2addr1-vnifilter 0
	setup-vm 1 21 2 10-v6-$hv1addr2-$hv2addr2-vnifilter,20-v6-$hv1addr2-$hv2addr2-vnifilter 0

	setup-vm 2 12 1 10-v4-$hv2addr1-$hv1addr1-vnifilter,20-v4-$hv2addr1-$hv1addr1-vnifilter 0
	setup-vm 2 22 2 10-v6-$hv2addr2-$hv1addr2-vnifilter,20-v6-$hv2addr2-$hv1addr2-vnifilter 0

        check_vm_connectivity "vnifiltering vxlan"
}

# Sanity test vnifilter datapath
# with vnifilter per vni configured BUM
# group/remote
vxlan_vnifilter_datapath_pervni()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"

	setup_ns hv_1 hv_2
	hv[1]=$hv_1
	hv[2]=$hv_2
	ip link add veth-hv-1 type veth peer name veth-hv-2
	setup-hv-networking 1 $hv1addr1 24 $hv1addr2 64
	setup-hv-networking 2 $hv2addr1 24 $hv2addr2 64

        check_hv_connectivity hv2addr1 hv2addr2

	setup_ns vm_11 vm_21 vm_12 vm_22
	vm[11]=$vm_11
	vm[21]=$vm_21
	vm[12]=$vm_12
	vm[22]=$vm_22
	setup-vm 1 11 1 10-v4-$hv1addr1-$hv2addr1-vnifilterg,20-v4-$hv1addr1-$hv2addr1-vnifilterg 0
	setup-vm 1 21 2 10-v6-$hv1addr2-$hv2addr2-vnifilterg,20-v6-$hv1addr2-$hv2addr2-vnifilterg 0

	setup-vm 2 12 1 10-v4-$hv2addr1-$hv1addr1-vnifilterg,20-v4-$hv2addr1-$hv1addr1-vnifilterg 0
	setup-vm 2 22 2 10-v6-$hv2addr2-$hv1addr2-vnifilterg,20-v6-$hv2addr2-$hv1addr2-vnifilterg 0

        check_vm_connectivity "vnifiltering vxlan pervni remote"
}


vxlan_vnifilter_datapath_mgroup()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"
        group="239.1.1.100"
        group6="ff07::1"

	setup_ns hv_1 hv_2
	hv[1]=$hv_1
	hv[2]=$hv_2
	ip link add veth-hv-1 type veth peer name veth-hv-2
	setup-hv-networking 1 $hv1addr1 24 $hv1addr2 64
	setup-hv-networking 2 $hv2addr1 24 $hv2addr2 64

        check_hv_connectivity hv2addr1 hv2addr2

	setup_ns vm_11 vm_21 vm_12 vm_22
	vm[11]=$vm_11
	vm[21]=$vm_21
	vm[12]=$vm_12
	vm[22]=$vm_22
	setup-vm 1 11 1 10-v4-$hv1addr1-$group-vnifilter,20-v4-$hv1addr1-$group-vnifilter 1
	setup-vm 1 21 2 "10-v6-$hv1addr2-$group6-vnifilter,20-v6-$hv1addr2-$group6-vnifilter" 1

        setup-vm 2 12 1 10-v4-$hv2addr1-$group-vnifilter,20-v4-$hv2addr1-$group-vnifilter 1
        setup-vm 2 22 2 10-v6-$hv2addr2-$group6-vnifilter,20-v6-$hv2addr2-$group6-vnifilter 1

        check_vm_connectivity "vnifiltering vxlan mgroup"
}

vxlan_vnifilter_datapath_mgroup_pervni()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"
        group="239.1.1.100"
        group6="ff07::1"

	setup_ns hv_1 hv_2
	hv[1]=$hv_1
	hv[2]=$hv_2
	ip link add veth-hv-1 type veth peer name veth-hv-2
	setup-hv-networking 1 $hv1addr1 24 $hv1addr2 64
	setup-hv-networking 2 $hv2addr1 24 $hv2addr2 64

        check_hv_connectivity hv2addr1 hv2addr2

	setup_ns vm_11 vm_21 vm_12 vm_22
	vm[11]=$vm_11
	vm[21]=$vm_21
	vm[12]=$vm_12
	vm[22]=$vm_22
	setup-vm 1 11 1 10-v4-$hv1addr1-$group-vnifilterg,20-v4-$hv1addr1-$group-vnifilterg 1
	setup-vm 1 21 2 10-v6-$hv1addr2-$group6-vnifilterg,20-v6-$hv1addr2-$group6-vnifilterg 1

        setup-vm 2 12 1 10-v4-$hv2addr1-$group-vnifilterg,20-v4-$hv2addr1-$group-vnifilterg 1
        setup-vm 2 22 2 10-v6-$hv2addr2-$group6-vnifilterg,20-v6-$hv2addr2-$group6-vnifilterg 1

        check_vm_connectivity "vnifiltering vxlan pervni mgroup"
}

vxlan_vnifilter_metadata_and_traditional_mix()
{
	hv1addr1="172.16.0.1"
	hv2addr1="172.16.0.2"
	hv1addr2="2002:fee1::1"
	hv2addr2="2002:fee1::2"

	setup_ns hv_1 hv_2
	hv[1]=$hv_1
	hv[2]=$hv_2
	ip link add veth-hv-1 type veth peer name veth-hv-2
	setup-hv-networking 1 $hv1addr1 24 $hv1addr2 64
	setup-hv-networking 2 $hv2addr1 24 $hv2addr2 64

        check_hv_connectivity hv2addr1 hv2addr2

	setup_ns vm_11 vm_21 vm_31 vm_12 vm_22 vm_32
	vm[11]=$vm_11
	vm[21]=$vm_21
	vm[31]=$vm_31
	vm[12]=$vm_12
	vm[22]=$vm_22
	vm[32]=$vm_32
	setup-vm 1 11 1 10-v4-$hv1addr1-$hv2addr1-vnifilter,20-v4-$hv1addr1-$hv2addr1-vnifilter 0
	setup-vm 1 21 2 10-v6-$hv1addr2-$hv2addr2-vnifilter,20-v6-$hv1addr2-$hv2addr2-vnifilter 0
	setup-vm 1 31 3 30-v4-$hv1addr1-$hv2addr1-default-4790,40-v6-$hv1addr2-$hv2addr2-default-4790,50-v4-$hv1addr1-$hv2addr1-metadata-4791 0


	setup-vm 2 12 1 10-v4-$hv2addr1-$hv1addr1-vnifilter,20-v4-$hv2addr1-$hv1addr1-vnifilter 0
	setup-vm 2 22 2 10-v6-$hv2addr2-$hv1addr2-vnifilter,20-v6-$hv2addr2-$hv1addr2-vnifilter 0
	setup-vm 2 32 3 30-v4-$hv2addr1-$hv1addr1-default-4790,40-v6-$hv2addr2-$hv1addr2-default-4790,50-v4-$hv2addr1-$hv1addr1-metadata-4791 0

        check_vm_connectivity "vnifiltering vxlan pervni remote mix"

	# check VM connectivity over traditional/non-vxlan filtering vxlan devices
	run_cmd "ip netns exec $vm_31 ping -c 1 -W 1 10.0.30.32"
        log_test $? 0 "VM connectivity over traditional vxlan (ipv4 default rdst)"

	run_cmd "ip netns exec $vm_31 ping -c 1 -W 1 10.0.40.32"
        log_test $? 0 "VM connectivity over traditional vxlan (ipv6 default rdst)"

	run_cmd "ip netns exec $vm_31 ping -c 1 -W 1 10.0.50.32"
        log_test $? 0 "VM connectivity over metadata nonfiltering vxlan (ipv4 default rdst)"
}

while getopts :t:pP46hv o
do
	case $o in
		t) TESTS=$OPTARG;;
		p) PAUSE_ON_FAIL=yes;;
		P) PAUSE=yes;;
		v) VERBOSE=$(($VERBOSE + 1));;
		h) usage; exit 0;;
		*) usage; exit 1;;
	esac
done

# make sure we don't pause twice
[ "${PAUSE}" = "yes" ] && PAUSE_ON_FAIL=no

if [ "$(id -u)" -ne 0 ];then
	echo "SKIP: Need root privileges"
	exit $ksft_skip;
fi

if [ ! -x "$(command -v ip)" ]; then
	echo "SKIP: Could not run test without ip tool"
	exit $ksft_skip
fi

ip link help vxlan 2>&1 | grep -q "vnifilter"
if [ $? -ne 0 ]; then
   echo "SKIP: iproute2 too old, missing vxlan dev vnifilter setting"
   sync
   exit $ksft_skip
fi

bridge vni help 2>&1 | grep -q "Usage: bridge vni"
if [ $? -ne 0 ]; then
   echo "SKIP: iproute2 bridge lacks vxlan vnifiltering support"
   exit $ksft_skip
fi

# start clean
cleanup &> /dev/null

for t in $TESTS
do
	case $t in
	none) setup; exit 0;;
	*) $t; cleanup;;
	esac
done

if [ "$TESTS" != "none" ]; then
	printf "\nTests passed: %3d\n" ${nsuccess}
	printf "Tests failed: %3d\n"   ${nfail}
fi

exit $ret
