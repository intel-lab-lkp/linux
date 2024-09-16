#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Pinned version of pmtu.sh pmtu_ipv6_ipv6_exception and all dependencies
# configured to reproduce a kernel bug where the veth_A-R1 device's
# resource counter remains > 0 after cleanup should have already been
# completed.
#
# On affected kernels/platforms, running this test will result in the following
# being displayed repeatedly in dmesg:
# unregister_netdevice: waiting for veth_A-R1 to become free. Usage count = 5
#
# and future attempts to modprobe/rmmod ip6_vti will hang forever.
#
# Link: https://lore.kernel.org/all/CAHTA-uZDaJ-71o+bo8a96TV4ck-8niimztQFaa=QoeNdUm-9wg@mail.gmail.com/
#
# BugLink: https://bugs.launchpad.net/ubuntu-kernel-tests/+bug/2072501
#
# pmtu.sh:
# Check that route PMTU values match expectations, and that initial device MTU
# values are assigned correctly
#
# Tests currently implemented:
#
# - pmtu_bad
# 	Sets the CPU governor to "performance" for all CPUs, then
# 	runs a pinned, affected version of the pmtu_ipv6_ipv6_exception test with
# 	nexthop objects 100 times. If this causes the following to be output
# 	to dmesg, the test is considered to have failed and returns an error:
#
# 	unregister_netdevice: waiting for veth_A-R1 to become free
#
# 	Otherwise, the test passes. After execution of the test, the CPU governor
# 	is restored to its original settings.
# - pmtu_ipv6_ipv6_exception
#	Same as pmtu_ipv4_vxlan4, but using a IPv4/IPv6 tunnel over IPv4/IPv6,
#	instead of VXLAN

# Pinned version of lib.sh
##############################################################################
# Defines

WAIT_TIMEOUT=${WAIT_TIMEOUT:=20}
BUSYWAIT_TIMEOUT=$((WAIT_TIMEOUT * 1000)) # ms

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
# namespace list created by setup_ns
NS_LIST=()

##############################################################################
# Helpers

__ksft_status_merge()
{
	local a=$1; shift
	local b=$1; shift
	local -A weights
	local weight=0

	local i
	for i in "$@"; do
		weights[$i]=$((weight++))
	done

	if [[ ${weights[$a]} > ${weights[$b]} ]]; then
		echo "$a"
		return 0
	else
		echo "$b"
		return 1
	fi
}

ksft_status_merge()
{
	local a=$1; shift
	local b=$1; shift

	__ksft_status_merge "$a" "$b" \
		$ksft_pass $ksft_xfail $ksft_skip $ksft_fail
}

ksft_exit_status_merge()
{
	local a=$1; shift
	local b=$1; shift

	__ksft_status_merge "$a" "$b" \
		$ksft_xfail $ksft_pass $ksft_skip $ksft_fail
}

loopy_wait()
{
	local sleep_cmd=$1; shift
	local timeout_ms=$1; shift

	local start_time="$(date -u +%s%3N)"
	while true
	do
		local out
		if out=$("$@"); then
			echo -n "$out"
			return 0
		fi

		local current_time="$(date -u +%s%3N)"
		if ((current_time - start_time > timeout_ms)); then
			echo -n "$out"
			return 1
		fi

		$sleep_cmd
	done
}

busywait()
{
	local timeout_ms=$1; shift

	loopy_wait : "$timeout_ms" "$@"
}

# timeout in seconds
slowwait()
{
	local timeout_sec=$1; shift

	loopy_wait "sleep 0.1" "$((timeout_sec * 1000))" "$@"
}

until_counter_is()
{
	local expr=$1; shift
	local current=$("$@")

	echo $((current))
	((current $expr))
}

busywait_for_counter()
{
	local timeout=$1; shift
	local delta=$1; shift

	local base=$("$@")
	busywait "$timeout" until_counter_is ">= $((base + delta))" "$@"
}

slowwait_for_counter()
{
	local timeout=$1; shift
	local delta=$1; shift

	local base=$("$@")
	slowwait "$timeout" until_counter_is ">= $((base + delta))" "$@"
}

cleanup_ns()
{
	local ns=""
	local errexit=0
	local ret=0

	# disable errexit temporary
	if [[ $- =~ "e" ]]; then
		errexit=1
		set +e
	fi

	for ns in "$@"; do
		[ -z "${ns}" ] && continue
		ip netns delete "${ns}" &> /dev/null
		if ! busywait $BUSYWAIT_TIMEOUT ip netns list \| grep -vq "^$ns$" &> /dev/null; then
			echo "Warn: Failed to remove namespace $ns"
			ret=1
		fi
	done

	[ $errexit -eq 1 ] && set -e
	return $ret
}

cleanup_all_ns()
{
	cleanup_ns "${NS_LIST[@]}"
}

# setup netns with given names as prefix. e.g
# setup_ns local remote
setup_ns()
{
	local ns=""
	local ns_name=""
	local ns_list=()
	local ns_exist=
	for ns_name in "$@"; do
		# Some test may setup/remove same netns multi times
		if unset ${ns_name} 2> /dev/null; then
			ns="${ns_name,,}-$(mktemp -u XXXXXX)"
			eval readonly ${ns_name}="$ns"
			ns_exist=false
		else
			eval ns='$'${ns_name}
			cleanup_ns "$ns"
			ns_exist=true
		fi

		if ! ip netns add "$ns"; then
			echo "Failed to create namespace $ns_name"
			cleanup_ns "${ns_list[@]}"
			return $ksft_skip
		fi
		ip -n "$ns" link set lo up
		! $ns_exist && ns_list+=("$ns")
	done
	NS_LIST+=("${ns_list[@]}")
}

tc_rule_stats_get()
{
	local dev=$1; shift
	local pref=$1; shift
	local dir=$1; shift
	local selector=${1:-.packets}; shift

	tc -j -s filter show dev $dev ${dir:-ingress} pref $pref \
	    | jq ".[1].options.actions[].stats$selector"
}

tc_rule_handle_stats_get()
{
	local id=$1; shift
	local handle=$1; shift
	local selector=${1:-.packets}; shift
	local netns=${1:-""}; shift

	tc $netns -j -s filter show $id \
	    | jq ".[] | select(.options.handle == $handle) | \
		  .options.actions[0].stats$selector"
}

# Pinned version of net_helper.sh
wait_local_port_listen()
{
	local listener_ns="${1}"
	local port="${2}"
	local protocol="${3}"
	local pattern
	local i

	pattern=":$(printf "%04X" "${port}") "

	# for tcp protocol additionally check the socket state
	[ ${protocol} = "tcp" ] && pattern="${pattern}0A"
	for i in $(seq 10); do
		if ip netns exec "${listener_ns}" awk '{print $2" "$4}' \
		   /proc/net/"${protocol}"* | grep -q "${pattern}"; then
			break
		fi
		sleep 0.1
	done
}



PAUSE_ON_FAIL=no
VERBOSE=0
TRACING=0

# Some systems don't have a ping6 binary anymore
which ping6 > /dev/null 2>&1 && ping6=$(which ping6) || ping6=$(which ping)

#               Name                          Description                  re-run with nh
tests="
	pmtu_bad	Runs IPv6 over IPv6: PMTU exceptions 100x w/ performance governor		1"

# Addressing and routing for tests with routers: four network segments, with
# index SEGMENT between 1 and 4, a common prefix (PREFIX4 or PREFIX6) and an
# identifier ID, which is 1 for hosts (A and B), 2 for routers (R1 and R2).
# Addresses are:
# - IPv4: PREFIX4.SEGMENT.ID (/24)
# - IPv6: PREFIX6:SEGMENT::ID (/64)
prefix4="10.0"
prefix6="fc00"
a_r1=1
a_r2=2
b_r1=3
b_r2=4
#	ns	peer	segment
routing_addrs="
	A	R1	${a_r1}
	A	R2	${a_r2}
	B	R1	${b_r1}
	B	R2	${b_r2}
"
# Traffic from A to B goes through R1 by default, and through R2, if destined to
# B's address on the b_r2 segment.
# Traffic from B to A goes through R1.
#	ns	destination		gateway
routes="
	A	default			${prefix4}.${a_r1}.2
	A	${prefix4}.${b_r2}.1	${prefix4}.${a_r2}.2
	B	default			${prefix4}.${b_r1}.2

	A	default			${prefix6}:${a_r1}::2
	A	${prefix6}:${b_r2}::1	${prefix6}:${a_r2}::2
	B	default			${prefix6}:${b_r1}::2
"
USE_NH="no"
#	ns	family	nh id	   destination		gateway
nexthops="
	A	4	41	${prefix4}.${a_r1}.2	veth_A-R1
	A	4	42	${prefix4}.${a_r2}.2	veth_A-R2
	B	4	41	${prefix4}.${b_r1}.2	veth_B-R1

	A	6	61	${prefix6}:${a_r1}::2	veth_A-R1
	A	6	62	${prefix6}:${a_r2}::2	veth_A-R2
	B	6	61	${prefix6}:${b_r1}::2	veth_B-R1
"

# nexthop id correlates to id in nexthops config above
#	ns    family	prefix			nh id
routes_nh="
	A	4	default			41
	A	4	${prefix4}.${b_r2}.1	42
	B	4	default			41

	A	6	default			61
	A	6	${prefix6}:${b_r2}::1	62
	B	6	default			61
"

policy_mark=0x04
rt_table=main

veth4_a_addr="192.168.1.1"
veth4_b_addr="192.168.1.2"
veth4_c_addr="192.168.2.10"
veth4_mask="24"
veth6_a_addr="fd00:1::a"
veth6_b_addr="fd00:1::b"
veth6_c_addr="fd00:2::c"
veth6_mask="64"

tunnel4_a_addr="192.168.2.1"
tunnel4_b_addr="192.168.2.2"
tunnel4_mask="24"
tunnel6_a_addr="fd00:2::a"
tunnel6_b_addr="fd00:2::b"
tunnel6_mask="64"

dummy6_0_prefix="fc00:1000::"
dummy6_1_prefix="fc00:1001::"
dummy6_mask="64"

err_buf=
tcpdump_pids=
nettest_pids=
socat_pids=
tmpoutfile=

err() {
	err_buf="${err_buf}${1}
"
}

err_flush() {
	echo -n "${err_buf}"
	err_buf=
}

run_cmd() {
	cmd="$*"

	if [ "$VERBOSE" = "1" ]; then
		printf "    COMMAND: $cmd\n"
	fi

	out="$($cmd 2>&1)"
	rc=$?
	if [ "$VERBOSE" = "1" -a -n "$out" ]; then
		echo "    $out"
		echo
	fi

	return $rc
}

run_cmd_bg() {
	cmd="$*"

	if [ "$VERBOSE" = "1" ]; then
		printf "    COMMAND: %s &\n" "${cmd}"
	fi

	$cmd 2>&1 &
}

# Find the auto-generated name for this namespace
nsname() {
	eval echo \$NS_$1
}

setup_ipvX_over_ipvY() {
	inner=${1}
	outer=${2}

	if [ "${outer}" -eq 4 ]; then
		a_addr="${prefix4}.${a_r1}.1"
		b_addr="${prefix4}.${b_r1}.1"
		if [ "${inner}" -eq 4 ]; then
			type="ipip"
			mode="ipip"
		else
			type="sit"
			mode="ip6ip"
		fi
	else
		a_addr="${prefix6}:${a_r1}::1"
		b_addr="${prefix6}:${b_r1}::1"
		type="ip6tnl"
		if [ "${inner}" -eq 4 ]; then
			mode="ipip6"
		else
			mode="ip6ip6"
		fi
	fi

	run_cmd ${ns_a} ip link add ip_a type ${type} local ${a_addr} remote ${b_addr} mode ${mode} || return $ksft_skip
	run_cmd ${ns_b} ip link add ip_b type ${type} local ${b_addr} remote ${a_addr} mode ${mode}

	run_cmd ${ns_a} ip link set ip_a up
	run_cmd ${ns_b} ip link set ip_b up

	if [ "${inner}" = "4" ]; then
		run_cmd ${ns_a} ip addr add ${tunnel4_a_addr}/${tunnel4_mask} dev ip_a
		run_cmd ${ns_b} ip addr add ${tunnel4_b_addr}/${tunnel4_mask} dev ip_b
	else
		run_cmd ${ns_a} ip addr add ${tunnel6_a_addr}/${tunnel6_mask} dev ip_a
		run_cmd ${ns_b} ip addr add ${tunnel6_b_addr}/${tunnel6_mask} dev ip_b
	fi
}

setup_ip6ip6() {
	setup_ipvX_over_ipvY 6 6
}

setup_namespaces() {
	setup_ns NS_A NS_B NS_C NS_R1 NS_R2
	for n in ${NS_A} ${NS_B} ${NS_C} ${NS_R1} ${NS_R2}; do
		# Disable DAD, so that we don't have to wait to use the
		# configured IPv6 addresses
		ip netns exec ${n} sysctl -q net/ipv6/conf/default/accept_dad=0
	done
	ns_a="ip netns exec ${NS_A}"
	ns_b="ip netns exec ${NS_B}"
	ns_c="ip netns exec ${NS_C}"
	ns_r1="ip netns exec ${NS_R1}"
	ns_r2="ip netns exec ${NS_R2}"
}


setup_routing_old() {
	for i in ${routes}; do
		[ "${ns}" = "" ]	&& ns="${i}"		&& continue
		[ "${addr}" = "" ]	&& addr="${i}"		&& continue
		[ "${gw}" = "" ]	&& gw="${i}"

		ns_name="$(nsname ${ns})"

		ip -n "${ns_name}" route add "${addr}" table "${rt_table}" via "${gw}"

		ns=""; addr=""; gw=""
	done
}

setup_routing_new() {
	for i in ${nexthops}; do
		[ "${ns}" = "" ]	&& ns="${i}"		&& continue
		[ "${fam}" = "" ]	&& fam="${i}"		&& continue
		[ "${nhid}" = "" ]	&& nhid="${i}"		&& continue
		[ "${gw}" = "" ]	&& gw="${i}"		&& continue
		[ "${dev}" = "" ]	&& dev="${i}"

		ns_name="$(nsname ${ns})"

		ip -n ${ns_name} -${fam} nexthop add id ${nhid} via ${gw} dev ${dev}

		ns=""; fam=""; nhid=""; gw=""; dev=""

	done

	for i in ${routes_nh}; do
		[ "${ns}" = "" ]	&& ns="${i}"		&& continue
		[ "${fam}" = "" ]	&& fam="${i}"		&& continue
		[ "${addr}" = "" ]	&& addr="${i}"		&& continue
		[ "${nhid}" = "" ]	&& nhid="${i}"

		ns_name="$(nsname ${ns})"

		ip -n "${ns_name}" -"${fam}" route add "${addr}" table "${rt_table}" nhid "${nhid}"

		ns=""; fam=""; addr=""; nhid=""
	done
}

setup_routing() {
	for i in ${NS_R1} ${NS_R2}; do
		ip netns exec ${i} sysctl -q net/ipv4/ip_forward=1
		ip netns exec ${i} sysctl -q net/ipv6/conf/all/forwarding=1
	done

	for i in ${routing_addrs}; do
		[ "${ns}" = "" ]	&& ns="${i}"		&& continue
		[ "${peer}" = "" ]	&& peer="${i}"		&& continue
		[ "${segment}" = "" ]	&& segment="${i}"

		ns_name="$(nsname ${ns})"
		peer_name="$(nsname ${peer})"
		if="veth_${ns}-${peer}"
		ifpeer="veth_${peer}-${ns}"

		# Create veth links
		ip link add ${if} up netns ${ns_name} type veth peer name ${ifpeer} netns ${peer_name} || return 1
		ip -n ${peer_name} link set dev ${ifpeer} up

		# Add addresses
		ip -n ${ns_name}   addr add ${prefix4}.${segment}.1/24  dev ${if}
		ip -n ${ns_name}   addr add ${prefix6}:${segment}::1/64 dev ${if}

		ip -n ${peer_name} addr add ${prefix4}.${segment}.2/24  dev ${ifpeer}
		ip -n ${peer_name} addr add ${prefix6}:${segment}::2/64 dev ${ifpeer}

		ns=""; peer=""; segment=""
	done

	if [ "$USE_NH" = "yes" ]; then
		setup_routing_new
	else
		setup_routing_old
	fi

	return 0
}


setup() {
	[ "$(id -u)" -ne 0 ] && echo "  need to run as root" && return $ksft_skip

	for arg do
		eval setup_${arg} || { echo "  ${arg} not supported"; return 1; }
	done
}

trace() {
	[ $TRACING -eq 0 ] && return

	for arg do
		[ "${ns_cmd}" = "" ] && ns_cmd="${arg}" && continue
		${ns_cmd} tcpdump --immediate-mode -s 0 -i "${arg}" -w "${name}_${arg}.pcap" 2> /dev/null &
		tcpdump_pids="${tcpdump_pids} $!"
		ns_cmd=
	done
	sleep 1
}


restore_governors() {
	echo "Restoring original CPU governors"
	while IFS=' ' read -r cpu governor; do
		echo "$governor" | tee "/sys/devices/system/cpu/$cpu/cpufreq/scaling_governor" > /dev/null
	done < "$GOVERNOR_STATE_FILE"
	rm -rf "$STATE_FILE_DIR" 2>/dev/null
}


cleanup() {
	for pid in ${tcpdump_pids}; do
		kill ${pid}
	done
	tcpdump_pids=

	for pid in ${nettest_pids}; do
		kill ${pid}
	done
	nettest_pids=

	for pid in ${socat_pids}; do
		kill "${pid}"
	done
	socat_pids=

	cleanup_all_ns

	ip link del veth_A-C			2>/dev/null
	ip link del veth_A-R1			2>/dev/null
	ovs-vsctl --if-exists del-port vxlan_a	2>/dev/null
	ovs-vsctl --if-exists del-br ovs_br0	2>/dev/null
	rm -f "$tmpoutfile"
}

mtu() {
	ns_cmd="${1}"
	dev="${2}"
	mtu="${3}"

	${ns_cmd} ip link set dev ${dev} mtu ${mtu}
}

mtu_parse() {
	input="${1}"

	next=0
	for i in ${input}; do
		[ ${next} -eq 1 -a "${i}" = "lock" ] && next=2 && continue
		[ ${next} -eq 1 ] && echo "${i}" && return
		[ ${next} -eq 2 ] && echo "lock ${i}" && return
		[ "${i}" = "mtu" ] && next=1
	done
}

link_get() {
	ns_cmd="${1}"
	name="${2}"

	${ns_cmd} ip link show dev "${name}"
}

link_get_mtu() {
	ns_cmd="${1}"
	name="${2}"

	mtu_parse "$(link_get "${ns_cmd}" ${name})"
}

route_get_dst_exception() {
	ns_cmd="${1}"
	dst="${2}"
	dsfield="${3}"

	if [ -z "${dsfield}" ]; then
		dsfield=0
	fi

	${ns_cmd} ip route get "${dst}" dsfield "${dsfield}"
}

route_get_dst_pmtu_from_exception() {
	ns_cmd="${1}"
	dst="${2}"
	dsfield="${3}"

	mtu_parse "$(route_get_dst_exception "${ns_cmd}" "${dst}" "${dsfield}")"
}

check_pmtu_value() {
	expected="${1}"
	value="${2}"
	event="${3}"

	[ "${expected}" = "any" ] && [ -n "${value}" ] && return 0
	[ "${value}" = "${expected}" ] && return 0
	[ -z "${value}" ] &&    err "  PMTU exception wasn't created after ${event}" && return 1
	[ -z "${expected}" ] && err "  PMTU exception shouldn't exist after ${event}" && return 1
	err "  found PMTU exception with incorrect MTU ${value}, expected ${expected}, after ${event}"
	return 1
}


test_pmtu_ipvX_over_ipvY_exception() {
	inner=${1}
	outer=${2}
	ll_mtu=4000

	setup namespaces routing ip${inner}ip${outer} || return $ksft_skip

	trace "${ns_a}" ip_a         "${ns_b}"  ip_b  \
	      "${ns_a}" veth_A-R1    "${ns_r1}" veth_R1-A \
	      "${ns_b}" veth_B-R1    "${ns_r1}" veth_R1-B

	if [ ${inner} -eq 4 ]; then
		ping=ping
		dst=${tunnel4_b_addr}
	else
		ping=${ping6}
		dst=${tunnel6_b_addr}
	fi

	if [ ${outer} -eq 4 ]; then
		#                      IPv4 header
		exp_mtu=$((${ll_mtu} - 20))
	else
		#                      IPv6 header   Option 4
		exp_mtu=$((${ll_mtu} - 40          - 8))
	fi

	# Create route exception by exceeding link layer MTU
	mtu "${ns_a}"  veth_A-R1 $((${ll_mtu} + 1000))
	mtu "${ns_r1}" veth_R1-A $((${ll_mtu} + 1000))
	mtu "${ns_b}"  veth_B-R1 ${ll_mtu}
	mtu "${ns_r1}" veth_R1-B ${ll_mtu}

	mtu "${ns_a}" ip_a $((${ll_mtu} + 1000)) || return
	mtu "${ns_b}" ip_b $((${ll_mtu} + 1000)) || return
	run_cmd ${ns_a} ${ping} -q -M want -i 0.1 -w 1 -s $((${ll_mtu} + 500)) ${dst}

	# Check that exception was created
	pmtu="$(route_get_dst_pmtu_from_exception "${ns_a}" ${dst})"
	check_pmtu_value ${exp_mtu} "${pmtu}" "exceeding link layer MTU on ip${inner}ip${outer} interface"
}

test_pmtu_ipv6_ipv6_exception() {
	test_pmtu_ipvX_over_ipvY_exception 6 6
}

run_test() {
	(
	tname="$1"
	tdesc="$2"

	unset IFS

	# Since cleanup() relies on variables modified by this subshell, it
	# has to run in this context.
	trap cleanup EXIT

	if [ "$VERBOSE" = "1" ]; then
		printf "\n##########################################################################\n\n"
	fi

	eval test_${tname}
	ret=$?

	if [ $ret -eq 0 ]; then
		printf "TEST: %-60s  [ OK ]\n" "${tdesc}"
	elif [ $ret -eq 1 ]; then
		printf "TEST: %-60s  [FAIL]\n" "${tdesc}"
		if [ "${PAUSE_ON_FAIL}" = "yes" ]; then
			echo
			echo "Pausing. Hit enter to continue"
			read a
		fi
		err_flush
		exit 1
	elif [ $ret -eq $ksft_skip ]; then
		printf "TEST: %-60s  [SKIP]\n" "${tdesc}"
		err_flush
	fi

	return $ret
	)
	ret=$?
	case $ret in
		0)
			all_skipped=false
			[ $exitcode -eq $ksft_skip ] && exitcode=0
		;;
		$ksft_skip)
			[ $all_skipped = true ] && exitcode=$ksft_skip
		;;
		*)
			all_skipped=false
			exitcode=1
		;;
	esac

	return $ret
}

run_test_nh() {
	tname="$1"
	tdesc="$2"

	USE_NH=yes
	run_test "${tname}" "${tdesc} - nexthop objects"
	USE_NH=no
}

usage() {
	echo
	echo "$0 [OPTIONS]..."
	echo "Runs pmtu_ipv6_ipv6_exception test 100x"
	echo
	echo "Options"
	echo "  --trace: capture traffic to TEST_INTERFACE.pcap"
	echo
	echo "Available tests${tests}"
	exit 1
}

################################################################################
#

[ "$(id -u)" -ne 0 ] && echo "ERROR: need to run as root" && exit 1
exitcode=0
desc=0
all_skipped=true

while getopts :ptv o
do
	case $o in
	p) PAUSE_ON_FAIL=yes;;
	v) VERBOSE=1;;
	t) if which tcpdump > /dev/null 2>&1; then
		TRACING=1
	   else
		echo "=== tcpdump not available, tracing disabled"
	   fi
	   ;;
	*) usage;;
	esac
done
shift $(($OPTIND-1))

IFS="
"

for arg do
	# Check first that all requested tests are available before running any
	command -v > /dev/null "test_${arg}" || { echo "=== Test ${arg} not found"; usage; }
done

trap cleanup EXIT

# start clean
cleanup

HAVE_NH=no
ip nexthop ls >/dev/null 2>&1
[ $? -eq 0 ] && HAVE_NH=yes

name=""
desc=""
rerun_nh=0

# Set to performance governor so we can reproduce the bug
STATE_FILE_DIR=$(mktemp -d)
if [[ ! -d "$STATE_FILE_DIR" ]]; then
	echo "Could not create temp dir, skipping this test" >&2
	exit 1
fi

# Save current CPU governor state to temp file
GOVERNOR_STATE_FILE="$STATE_FILE_DIR/orig_cpu_governors.txt"
touch $GOVERNOR_STATE_FILE
if [[ ! -e "$GOVERNOR_STATE_FILE" ]]; then
	echo "Could not save current performance governor states, skipping this test" >&2
	exit 1
fi
echo "Saving current performance governor settings"
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
	if [[ $cpu =~ ^/sys/devices/system/cpu/(cpu[0-9]+)/cpufreq/scaling_governor$ ]]; then
		echo "${BASH_REMATCH[1]} $(cat $cpu)" >> "$GOVERNOR_STATE_FILE"
	fi
done

# Restore original CPU governor settings
trap restore_governors EXIT

echo "Switching CPU governor to performance"
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
	if [[ $cpu =~ ^/sys/devices/system/cpu/(cpu[0-9]+)/cpufreq/scaling_governor$ ]]; then
		echo "performance" | tee $cpu > /dev/null
	fi
done

for i in $(seq 1 100);
do
	name="pmtu_ipv6_ipv6_exception"
	desc="Bad PMTU behavior"

	if [ "${HAVE_NH}" = "yes" ]; then
		rerun_nh="${t}"
	fi

	run_this=1
	for arg do
		[ "${arg}" != "${arg#--*}" ] && continue
		[ "${arg}" = "${name}" ] && run_this=1 && break
		run_this=0
	done
	if [ $run_this -eq 1 ]; then
		run_test_nh "${name}" "${desc}"
	fi
	name=""
	desc=""
	rerun_nh=0
done

if dmesg | grep -q "unregister_netdevice: waiting for veth_A-R1 to become free"; then
	printf "TEST: Bad PMTU behavior - veth_A-R1 refcount error reproducer  [FAIL]\n"
	printf "veth_A-R1 has not been released properly\n"
	exitcode=1
else
	printf "TEST: Bad PMTU behavior - veth_A-R1 refcount error reproducer  [PASS]\n"
	printf "No veth_A-R1 errors, considering this test passed\n"
	exitcode=0
fi

exit ${exitcode}
