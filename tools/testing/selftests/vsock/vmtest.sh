#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2025 Meta Platforms, Inc. and affiliates
#
# Dependencies:
#		* virtme-ng
#		* busybox-static (used by virtme-ng)
#		* qemu	(used by virtme-ng)
#
# Namespace tests require to test the functionality of VSOCK under different
# namespace configurations. Ideally, we can use vsock_test and friends under
# the different configurations to ensure that all functionality works
# regardless of namespace setup. vsock_test also requires TCP for its control
# plane, which is also impacted by namespacing. For this reason, these tests
# build a bridge between the namespaces so that the TCP control traffic can
# flow between namespaces. The bridge setup looks as follows:
#
#
#                                  |
#     +------------------+         |
#     | VM               |         |
#     |                  |     NS0 | NS1
#     |  +------------+  |         |
#     |  |            |  | --------+--------------------+
#     |  | vsock_test |  |         |                    |
#     |  |            |  | <-------+-----------------+  |
#     |  +------------+  |         |  VSOCK_TEST_PORT|  |
#     |                  |         |                 |  | VSOCK
#     +------------------+         |                 |  |
#              ^  |                |                 |  |
#  CONTROL_PORT|  |                |                 |  |
#              |  |                |                 |  |
#              |  |                |                 |  v
#              |  |                |             +------------+
#              |  | TCP            |             |            |
#              |  |                |             | vsock_test |
#              |  |                |             |            |
#              |  |                |             +------------+
# CONTROL_PORT |  |                |   CONTROL_PORT  ^  |
#              |  |                |                 |  |
#              |  v                |   CONTROL_PORT  |  v
#           +-------+              |              +-------+
#           |       |veth0         |         veth1|       |
#           | socat |<-------------+------------- | socat |
#           |       | -------------+------------> |       |
#           +-------+              |              +-------+
#              NS_BRIDGE_PORT      |       NS_BRIDGE_PORT
#                                  |

set -u

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT=$(realpath "${SCRIPT_DIR}"/../../../../)

source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

readonly VSOCK_TEST="${SCRIPT_DIR}"/vsock_test
readonly TEST_GUEST_PORT=51000
readonly TEST_HOST_PORT=50000
readonly TEST_HOST_PORT_LISTENER=50001
readonly SSH_GUEST_PORT=22
readonly SSH_HOST_PORT=2222
readonly BRIDGE_PORT=5678
readonly DEFAULT_CID=1234
readonly WAIT_PERIOD=3
readonly WAIT_PERIOD_MAX=60
WAIT_TOTAL=$(( WAIT_PERIOD * WAIT_PERIOD_MAX ))

# virtme-ng offers a netdev for ssh when using "--ssh", but we also need a
# control port forwarded for vsock_test.  Because virtme-ng doesn't support
# adding an additional port to forward to the device created from "--ssh" and
# virtme-init mistakenly sets identical IPs to the ssh device and additional
# devices, we instead opt out of using --ssh, add the device manually, and also
# add the kernel cmdline options that virtme-init uses to setup the interface.
readonly QEMU_TEST_PORT_FWD="hostfwd=tcp::${TEST_HOST_PORT}-:${TEST_GUEST_PORT}"
readonly QEMU_SSH_PORT_FWD="hostfwd=tcp::${SSH_HOST_PORT}-:${SSH_GUEST_PORT}"
readonly LOG=$(mktemp /tmp/vsock_vmtest_XXXX.log)
readonly TEST_NAMES=(vm_server_host_client vm_client_host_server vm_loopback)
QEMU_OPTS="\
	 -netdev user,id=n0,${QEMU_TEST_PORT_FWD},${QEMU_SSH_PORT_FWD} \
	 -device virtio-net-pci,netdev=n0 \
"
readonly KERNEL_CMDLINE="\
	virtme.dhcp net.ifnames=0 biosdevname=0 \
	virtme.ssh virtme_ssh_channel=tcp virtme_ssh_user=$USER \
"
readonly LOG=$(mktemp /tmp/vsock_vmtest_XXXX.log)
readonly TEST_NAMES=(
	vm_server_host_client
	vm_client_host_server
	vm_loopback
	host_vsock_ns_mode
	host_vsock_ns_mode_write_once
	global_same_cid
	local_same_cid
	global_local_same_cid
	local_global_same_cid
	global_host_connect_global_vm
	global_vm_connect_global_host
	global_vm_connect_mixed_host
)

readonly TEST_DESCS=(
	"Run vsock_test in server mode on the VM and in client mode on the host."
	"Run vsock_test in client mode on the VM and in server mode on the host."
	"Run vsock_test using the loopback transport in the VM."
	"Check /proc/net/vsock_ns_mode strings on the host."
	"Check /proc/net/vsock_ns_mode is write-once on the host."
	"Test that CID allocation fails with the same CID, one global NS and another global NS."
	"Test that CID allocation succeeds with the same CID, one local NS and another local NS."
	"Test that CID allocation succeeds with the same CID, one global NS and one local NS, global allocates first."
	"Test that CID allocation succeeds with the same CID, one global NS and one local NS, local allocates first."
)
readonly NEEDS_SETUP=(vm_server_host_client vm_client_host_server vm_loopback)
readonly MODES=("local" "global" "mixed")
readonly PIDFILE_TEMPLATE="/tmp/qemu_vsock_vmtest_XXXX.pid"

declare -a PIDFILES

VERBOSE=0

usage() {
	local name
	local desc
	local i

	echo
	echo "$0 [OPTIONS] [TEST]..."
	echo "If no TEST argument is given, all tests will be run."
	echo
	echo "Options"
	echo "  -b: build the kernel from the current source tree and use it for guest VMs"
	echo "  -q: set the path to or name of qemu binary"
	echo "  -v: verbose output"
	echo
	echo "Available tests"

	for ((i = 0; i < ${#TEST_NAMES[@]}; i++)); do
		name=${TEST_NAMES[${i}]}
		desc=${TEST_DESCS[${i}]}
		printf "\t%-35s%-35s\n" "${name}" "${desc}"
	done
	echo

	exit 1
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
}

cleanup() {
	terminate_pidfiles ${PIDFILES[@]}
	del_namespaces
}

vm_ssh() {
	ssh -q -o UserKnownHostsFile=/dev/null -p ${SSH_HOST_PORT} localhost "$@"
	return $?
}

vm_ssh_ns() {
	local ns="${1}"
	local NS_EXEC="ip netns exec ${ns}"
	shift

	${NS_EXEC} ssh -q -o UserKnownHostsFile=/dev/null -p ${SSH_HOST_PORT} localhost $*

	return $?
}

terminate_pidfiles() {
	local pidfile

	for pidfile in $@; do
		if [[ -s "${pidfile}" ]]; then
			pkill -SIGTERM -F ${pidfile} 2>&1 > /dev/null
		fi

		# If failure occurred during or before qemu start up, then we need
		# to clean this up ourselves.
		if [[ -e "${pidfile}" ]]; then
			rm "${pidfile}"
		fi
	done
}

check_args() {
	local found

	for arg in "$@"; do
		found=0
		for name in "${TEST_NAMES[@]}"; do
			if [[ "${name}" = "${arg}" ]]; then
				found=1
				break
			fi
		done

		if [[ "${found}" -eq 0 ]]; then
			echo "${arg} is not an available test" >&2
			usage
		fi
	done

	for arg in "$@"; do
		if ! command -v > /dev/null "test_${arg}"; then
			echo "Test ${arg} not found" >&2
			usage
		fi
	done
}

check_deps() {
	for dep in vng ${QEMU} busybox pkill ssh; do
		if [[ ! -x $(command -v "${dep}") ]]; then
			echo -e "skip:    dependency ${dep} not found!\n"
			exit "${KSFT_SKIP}"
		fi
	done

	if [[ ! -x $(command -v "${VSOCK_TEST}") ]]; then
		printf "skip:    %s not found!" "${VSOCK_TEST}"
		printf " Please build the kselftest vsock target.\n"
		exit "${KSFT_SKIP}"
	fi
}

check_vng() {
	local tested_versions
	local version
	local ok

	tested_versions=("1.33" "1.36")
	version="$(vng --version)"

	ok=0
	for tv in "${tested_versions[@]}"; do
		if [[ "${version}" == *"${tv}"* ]]; then
			ok=1
			break
		fi
	done

	if [[ ! "${ok}" -eq 1 ]]; then
		printf "warning: vng version '%s' has not been tested and may " "${version}" >&2
		printf "not function properly.\n\tThe following versions have been tested: " >&2
		echo "${tested_versions[@]}" >&2
	fi
}

handle_build() {
	if [[ ! "${BUILD}" -eq 1 ]]; then
		return
	fi

	if [[ ! -d "${KERNEL_CHECKOUT}" ]]; then
		echo "-b requires vmtest.sh called from the kernel source tree" >&2
		exit 1
	fi

	pushd "${KERNEL_CHECKOUT}" &>/dev/null

	if ! vng --kconfig --config "${SCRIPT_DIR}"/config; then
		die "failed to generate .config for kernel source tree (${KERNEL_CHECKOUT})"
	fi

	if ! make -j$(nproc); then
		die "failed to build kernel from source tree (${KERNEL_CHECKOUT})"
	fi

	popd &>/dev/null
}

vm_start() {
	local cid=$1
	local ns=$2
	local verify_boot=${3:-1}
	local pidfile=${4:-}

	local logfile=/dev/null
	local qemu_opts=""
	local verbose_opt=""
	local kernel_opt=""
	local qemu

	qemu=$(command -v "${QEMU}")

	if [[ "${VERBOSE}" -eq 1 ]]; then
		verbose_opt="--verbose"
		logfile=/dev/stdout
	fi

	qemu_opts="\
		${QEMU_OPTS} -device vhost-vsock-pci,guest-cid=${cid} \
		--pidfile ${pidfile}
	"

	if [[ "${BUILD}" -eq 1 ]]; then
		kernel_opt="${KERNEL_CHECKOUT}"
	fi

	if [[ ! -z "${ns}" ]]; then
		NS_EXEC="ip netns exec ${ns}"
	fi

	if [[ -z "${pidfile}" ]]; then
		pidfile=$(mktemp $PIDFILE_TEMPLATE)
		PIDFILES+=("${pidfile}")
	fi

	${NS_EXEC} vng \
		--run \
		${kernel_opt} \
		${verbose_opt} \
		--qemu-opts="${qemu_opts}" \
		--qemu="${qemu}" \
		--user root \
		--append "${KERNEL_CMDLINE}" \
		--rw  &> ${logfile} &

	timeout ${WAIT_TOTAL} \
		bash -c 'while [[ ! -s '"${pidfile}"' ]]; do sleep 1; done; exit 0'
}

vm_wait_for_ssh() {
	local ns="${1}"
	local i

	i=0
	while [[ true ]]; do
		if [[ ${i} -gt ${WAIT_PERIOD_MAX} ]]; then
			die "Timed out waiting for guest ssh"
		fi
		if [[ ! -z "${ns}" ]]; then
			vm_ssh_ns "${ns}" -- true
		else
			vm_ssh -- true
		fi
		if [[ $? -eq 0 ]]; then
			break
		fi
		i=$(( i + 1 ))
		sleep ${WAIT_PERIOD}
	done
}

# derived from selftests/net/net_helper.sh
wait_for_listener()
{
	local port=$1
	local interval=$2
	local max_intervals=$3
	local protocol=tcp
	local pattern
	local i

	pattern=":$(printf "%04X" "${port}") "

	# for tcp protocol additionally check the socket state
	[ "${protocol}" = "tcp" ] && pattern="${pattern}0A"
	for i in $(seq "${max_intervals}"); do
		if awk '{print $2" "$4}' /proc/net/"${protocol}"* | \
		   grep -q "${pattern}"; then
			break
		fi
		sleep "${interval}"
	done
}

vm_wait_for_listener() {
	local port=$1
	local host_ns=$2

	vm_ssh_ns "${host_ns}" <<EOF
$(declare -f wait_for_listener)
wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
EOF
}

host_wait_for_listener() {
	wait_for_listener "${TEST_HOST_PORT_LISTENER}" "${WAIT_PERIOD}" "${WAIT_PERIOD_MAX}"
	wait_for_listener ${TEST_HOST_PORT_LISTENER} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
}

host_ns_wait_for_listener() {
	local ns="${1}"
	local port="${2}"

	ip netns exec "${ns}" bash <<-EOF
		$(declare -f wait_for_listener)
		wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
	EOF
}

__log_stdin() {
	cat | awk '{ printf "%s:\t%s\n","'"${prefix}"'", $0 }'
}

__log_args() {
	echo "$*" | awk '{ printf "%s:\t%s\n","'"${prefix}"'", $0 }'
}

log() {
	local prefix="$1"

	shift
	local redirect=
	if [[ ${VERBOSE} -eq 0 ]]; then
		redirect=/dev/null
	else
		redirect=/dev/stdout
	fi

	if [[ "$#" -eq 0 ]]; then
		__log_stdin | tee -a "${LOG}" > ${redirect}
	else
		__log_args "$@" | tee -a "${LOG}" > ${redirect}
	fi
}

log_setup() {
	log "setup" "$@"
}

log_host() {
	local testname=$1

	shift
	log "test:${testname}:host" "$@"
}

log_guest() {
	local testname=$1

	shift
	log "test:${testname}:guest" "$@"
}

test_vm_server_host_client() {
	local testname="${FUNCNAME[0]#test_}"

	vm_ssh -- "${VSOCK_TEST}" \
		--mode=server \
		--control-port="${TEST_GUEST_PORT}" \
		--peer-cid=2 \
		2>&1 | log_guest "${testname}" &

	vm_wait_for_listener "${TEST_GUEST_PORT}"

	${VSOCK_TEST} \
		--mode=client \
		--control-host=127.0.0.1 \
		--peer-cid="${DEFAULT_CID}" \
		--control-port="${TEST_HOST_PORT}" 2>&1 | log_host "${testname}"

	return $?
}

test_vm_client_host_server() {
	local testname="${FUNCNAME[0]#test_}"

	${VSOCK_TEST} \
		--mode "server" \
		--control-port "${TEST_HOST_PORT_LISTENER}" \
		--peer-cid "${DEFAULT_CID}" 2>&1 | log_host "${testname}" &

	host_wait_for_listener

	vm_ssh -- "${VSOCK_TEST}" \
		--mode=client \
		--control-host=10.0.2.2 \
		--peer-cid=2 \
		--control-port="${TEST_HOST_PORT_LISTENER}" 2>&1 | log_guest "${testname}"

	return $?
}

test_vm_loopback() {
	local testname="${FUNCNAME[0]#test_}"
	local port=60000 # non-forwarded local port

	vm_ssh -- "${VSOCK_TEST}" \
		--mode=server \
		--control-port="${port}" \
		--peer-cid=1 2>&1 | log_guest "${testname}" &

	vm_wait_for_listener "${port}"

	vm_ssh -- "${VSOCK_TEST}" \
		--mode=client \
		--control-host="127.0.0.1" \
		--control-port="${port}" \
		--peer-cid=1 2>&1 | log_guest "${testname}"

	return $?
}

add_namespaces() {
	local init=${1:-0}

	for mode in "${MODES[@]}"; do
		if ! ip netns add "${mode}"; then
			return ${KSFT_FAIL}
		fi

		# e.g., global-2, local-2, mixed-2
		if ! ip netns add "${mode}-2"; then
			return ${KSFT_FAIL}
		fi

		if [[ ${init} -eq 1 ]]; then
			ns_set_mode "${mode}" "${mode}"
			ns_set_mode "${mode}-2" "${mode}"

			# we need lo for qemu port forwarding
			ip netns exec "${mode}" ip link set dev lo up
			ip netns exec "${mode}-2" ip link set dev lo up
		fi
	done
	return 0
}

del_namespaces() {
	for mode in "${MODES[@]}"; do
		ip netns del "${mode}"
		ip netns del "${mode}-2"
	done &>/dev/null
}

ns_set_mode() {
	local ns=$1
	local mode=$2

	echo "${mode}" \
		| ip netns exec "${ns}" \
			tee /proc/net/vsock_ns_mode &>/dev/null
}

setup_bridge() {
	local ns0
	local ns1
	local addr1

	ns0=$1
	ns1=$2

	ip link add veth0 type veth peer name veth1
	ip link set veth0 netns "${ns0}"
	ip link set veth1 netns "${ns1}"
	ip netns exec "${ns0}" ip addr add 10.0.0.1/24 dev veth0
	ip netns exec "${ns1}" ip addr add 10.0.0.2/24 dev veth1
	ip netns exec "${ns0}" ip link set veth0 up
	ip netns exec "${ns1}" ip link set veth1 up
}

teardown_bridge() {
	local ns0="${1}"

	# veth1 is implicitly destroyed with veth0
	ip netns exec "${ns0}" ip link delete veth0
}

test_host_vsock_ns_mode() {
	if ! add_namespaces; then
		return ${KSFT_FAIL}
	fi

	for mode in "${MODES[@]}"; do
		if ! ns_set_mode "${mode}" "${mode}"; then
			del_namespaces
			return ${KSFT_FAIL}
		fi
	done

	if ! del_namespaces; then
		return ${KSFT_FAIL}
	fi
}

test_host_vsock_ns_mode_write_once() {
	if ! add_namespaces; then
		return ${KSFT_FAIL}
	fi

	for mode in "${MODES[@]}"; do
		if ! ns_set_mode "${mode}" "${mode}"; then
			del_namespaces
			return ${KSFT_FAIL}
		fi

		# try setting back to global, should fail
		if ns_set_mode "${mode}" "global"; then
			del_namespaces
			return ${KSFT_FAIL}
		fi
	done

	if ! del_namespaces; then
		return ${KSFT_FAIL}
	fi
}

namespaces_can_boot_same_cid() {
	local ns1=$1
	local ns2=$2
	local cid=20
	local pidfile1
	local pidfile2
	local msg

	if ! add_namespaces 1; then
		return 1
	fi

	if [[ ${VERBOSE} -gt 0 ]]; then
		echo "booting vm 1" | tap_prefix
	fi

	pidfile1=$(mktemp $PIDFILE_TEMPLATE)
	PIDFILES+=("${pidfile1}")
	vm_start ${cid} ${ns1} ${pidfile1}

	if [[ ${VERBOSE} -gt 0 ]]; then
		echo "booting vm 2" | tap_prefix
	fi

	pidfile2=$(mktemp $PIDFILE_TEMPLATE)
	PIDFILES+=("${pidfile2}")
	WAIT_TOTAL=30 vm_start ${cid} ${ns2} ${pidfile2}

	rc=$?
	if [[ $rc -eq 0 ]]; then
		msg="successfully booted"
		rc=0
	else
		msg="failed to boot"
		rc=1
	fi

	if [[ ${VERBOSE} -gt 0 ]]; then
		echo "vm 2 ${msg}" | tap_prefix
	fi
	if ! del_namespaces; then
		echo "failed to delete namespaces" | tap_prefix
	fi

	terminate_pidfiles ${pidfile1} ${pidfile2}
	return $rc
}

test_global_same_cid() {
	if namespaces_can_boot_same_cid "global" "global-2"; then
		return $KSFT_FAIL
	fi

	return $KSFT_PASS
}

test_local_global_same_cid() {
	if namespaces_can_boot_same_cid "local" "global"; then
		return $KSFT_PASS
	fi

	return $KSFT_FAIL
}

test_global_local_same_cid() {
	if namespaces_can_boot_same_cid "global" "local"; then
		return $KSFT_PASS
	fi

	return $KSFT_FAIL
}

test_local_same_cid() {
	if namespaces_can_boot_same_cid "local" "local"; then
		return $KSFT_FAIL
	fi

	return $KSFT_PASS
}

test_global_host_connect_global_vm() {
	local testname="${FUNCNAME[0]#test_}"
	local cid=${DEFAULT_CID}
	local port=1234
	local host_ns="global"
	local host_ns2="global-2"

	add_namespaces 1
	setup_bridge "${host_ns}" "${host_ns2}"

	# Start server in VM in namespace
	if ! vm_start ${cid} "${host_ns}"; then
		teardown_bridge "${host_ns}"
		return $KSFT_FAIL
	fi

	vm_ssh_ns "${host_ns}" \
		-- "${VSOCK_TEST}" \
		--mode=server \
		--control-port="${TEST_GUEST_PORT}" \
		--peer-cid=2 \
		2>&1 | log_guest "${testname}" &
	vm_wait_for_listener ${TEST_GUEST_PORT} "${host_ns}"

	# Setup NS-to-NS "bridge" 
	ip netns exec "${host_ns}" socat TCP-LISTEN:${BRIDGE_PORT},fork \
		TCP-CONNECT:localhost:${TEST_HOST_PORT} &
	host_ns_wait_for_listener "${host_ns}" "${BRIDGE_PORT}"

	ip netns exec "${host_ns2}" \
		socat TCP:10.0.0.1:${BRIDGE_PORT} TCP-LISTEN:${TEST_HOST_PORT},fork &
	host_ns_wait_for_listener "${host_ns2}" "${TEST_HOST_PORT}"

	# Start client in other namespace
	ip netns exec "${host_ns2}" ${VSOCK_TEST} \
		--mode=client \
		--control-host=127.0.0.1 \
		--peer-cid="${cid}" \
		--control-port="${TEST_HOST_PORT}" 2>&1 | log_host "${testname}"
	rc=$?

	if [[ ! $rc -eq 0 ]]; then
		return $KSFT_FAIL
	fi

	del_namespaces

	return $KSFT_PASS
}

do_ns_vm_client_host_server_test() {
	local testname="$1"
	local host_ns="$2"
	local host_ns2="$3"
	local cid=${DEFAULT_CID}

	# must not be same as qemu hostfwd port
	local port=12345

	add_namespaces 1
	setup_bridge "${host_ns}" "${host_ns2}"

	if ! vm_start ${cid} "${host_ns}"; then
		teardown_bridge "${host_ns}"
		return $KSFT_FAIL
	fi

	ip netns exec "${host_ns2}" ${VSOCK_TEST} \
		--mode=server \
		--peer-cid="${cid}" \
		--control-port="${port}" 2>&1 | log_host "${testname}" &

	host_ns_wait_for_listener "${host_ns2}" "${port}"

	ip netns exec "${host_ns2}" \
		socat TCP-LISTEN:${BRIDGE_PORT},bind=10.0.0.2,fork \
			TCP:localhost:${port} &

	host_ns_wait_for_listener "${host_ns2}" "${BRIDGE_PORT}"

	ip netns exec "${host_ns}" socat TCP-LISTEN:${port},fork \
		TCP-CONNECT:10.0.0.2:${BRIDGE_PORT} &

	host_ns_wait_for_listener "${host_ns}" "${port}"

	vm_ssh_ns "${host_ns}" \
		-- "${VSOCK_TEST}" \
		--mode=client \
		--control-host=10.0.2.2 \
		--control-port="${port}" \
		--peer-cid=2 \
		2>&1 | log_guest "${testname}"

	if [[ ! $? -eq 0 ]]; then
		return $KSFT_FAIL
	fi

	del_namespaces

	return $KSFT_PASS
}

test_global_vm_connect_global_host() {
	local testname="${FUNCNAME[0]#test_}"
	local host_ns="global"
	local host_ns2="global-2"

	do_ns_vm_client_host_server_test ${testname} ${host_ns} ${host_ns2}
}

test_global_vm_connect_mixed_host() {
	local testname="${FUNCNAME[0]#test_}"
	local host_ns="global"
	local host_ns2="mixed"

	do_ns_vm_client_host_server_test ${testname} ${host_ns} ${host_ns2}
}

run_test() {
	local host_oops_cnt_before
	local host_warn_cnt_before
	local vm_oops_cnt_before
	local vm_warn_cnt_before
	local host_oops_cnt_after
	local host_warn_cnt_after
	local vm_oops_cnt_after
	local vm_warn_cnt_after
	local name
	local rc

	host_oops_cnt_before=$(dmesg | grep -c -i 'Oops')
	host_warn_cnt_before=$(dmesg --level=warn | wc -l)
	vm_oops_cnt_before=$(vm_ssh -- dmesg | grep -c -i 'Oops')
	vm_warn_cnt_before=$(vm_ssh -- dmesg --level=warn | wc -l)

	name=$(echo "${1}" | awk '{ print $1 }')
	eval test_"${name}"
	rc=$?

	host_oops_cnt_after=$(dmesg | grep -i 'Oops' | wc -l)
	if [[ ${host_oops_cnt_after} -gt ${host_oops_cnt_before} ]]; then
		echo "FAIL: kernel oops detected on host" | log_host "${name}"
		rc=$KSFT_FAIL
	fi

	host_warn_cnt_after=$(dmesg --level=warn | wc -l)
	if [[ ${host_warn_cnt_after} -gt ${host_warn_cnt_before} ]]; then
		echo "FAIL: kernel warning detected on host" | log_host "${name}"
		rc=$KSFT_FAIL
	fi

	vm_oops_cnt_after=$(vm_ssh -- dmesg | grep -i 'Oops' | wc -l)
	if [[ ${vm_oops_cnt_after} -gt ${vm_oops_cnt_before} ]]; then
		echo "FAIL: kernel oops detected on vm" | log_host "${name}"
		rc=$KSFT_FAIL
	fi

	vm_warn_cnt_after=$(vm_ssh -- dmesg --level=warn | wc -l)
	if [[ ${vm_warn_cnt_after} -gt ${vm_warn_cnt_before} ]]; then
		echo "FAIL: kernel warning detected on vm" | log_host "${name}"
		rc=$KSFT_FAIL
	fi

	check_result "${rc}"
}

needs_setup() {
	local tname

	tname="$1"

	for testname in ${NEEDS_SETUP[@]}; do
		if [[ "${tname}" == "${testname}" ]]; then
			return 1
		fi
	done

	return 0
}

check_result() {
	local rc

	rc=$1

	if [[ ${rc} -eq $KSFT_PASS ]]; then
		cnt_pass=$(( cnt_pass + 1 ))
		echo "ok ${cnt_total} ${arg}"
	elif [[ ${rc} -eq $KSFT_SKIP ]]; then
		cnt_skip=$(( cnt_skip + 1 ))
		echo "ok ${cnt_total} ${arg} # SKIP"
	elif [[ ${rc} -eq $KSFT_FAIL ]]; then
		cnt_fail=$(( cnt_fail + 1 ))
		echo "not ok ${cnt_total} ${arg} # exit=$rc"
	fi

	cnt_total=$(( cnt_total + 1 ))
}

QEMU="qemu-system-$(uname -m)"

while getopts :hvsq:b o
do
	case $o in
	v) VERBOSE=1;;
	b) BUILD=1;;
	q) QEMU=$OPTARG;;
	h|*) usage;;
	esac
done
shift $((OPTIND-1))

trap cleanup EXIT

if [[ ${#} -eq 0 ]]; then
	ARGS=("${TEST_NAMES[@]}")
else
	ARGS=("$@")
fi

check_args "${ARGS[@]}"
check_deps
check_vng
handle_build

echo "1..${#ARGS[@]}"

cnt_pass=0
cnt_fail=0
cnt_skip=0
cnt_total=0
setup_done=0

pidfile=""
for arg in ${ARGS[@]}; do
	if needs_setup "${arg}"; then
		if [[ -z "${pidfile}" ]]; then
			pidfile=$(mktemp $PIDFILE_TEMPLATE)
			log_setup "Booting up VM"
			vm_start "${DEFAULT_CID}" "" "${pidfile}"
			vm_wait_for_ssh
			log_setup "VM booted up"
		fi

		run_test "${arg}"
	fi
done

if [[ ! -z "${pidfile}" ]]; then
	log_setup "VM terminate"
	terminate_pidfiles "${pidfile}"
fi

for arg in "${ARGS[@]}"; do
	if ! needs_setup "${arg}"; then
		run_test "${arg}"
	fi
done

echo "SUMMARY: PASS=${cnt_pass} SKIP=${cnt_skip} FAIL=${cnt_fail}"
echo "Log: ${LOG}"

if [ $((cnt_pass + cnt_skip)) -eq ${cnt_total} ]; then
	exit "$KSFT_PASS"
else
	exit "$KSFT_FAIL"
fi
