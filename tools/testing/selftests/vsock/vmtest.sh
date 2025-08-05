#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2025 Meta Platforms, Inc. and affiliates
#
# Dependencies:
#		* virtme-ng
#		* busybox-static (used by virtme-ng)
#		* qemu	(used by virtme-ng)
#		* socat

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT=$(realpath "${SCRIPT_DIR}"/../../../../)

source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

readonly VSOCK_TEST="${SCRIPT_DIR}"/vsock_test
readonly TEST_GUEST_PORT=51000
readonly TEST_HOST_PORT=50000
readonly TEST_HOST_PORT_LISTENER=50001
readonly SSH_GUEST_PORT=22
readonly SSH_HOST_PORT=2222
readonly VSOCK_CID=1234
readonly WAIT_PERIOD=3
readonly WAIT_PERIOD_MAX=60
readonly WAIT_TOTAL=$(( WAIT_PERIOD * WAIT_PERIOD_MAX ))
readonly WAIT_QEMU=5

# virtme-ng offers a netdev for ssh when using "--ssh", but we also need a
# control port forwarded for vsock_test.  Because virtme-ng doesn't support
# adding an additional port to forward to the device created from "--ssh" and
# virtme-init mistakenly sets identical IPs to the ssh device and additional
# devices, we instead opt out of using --ssh, add the device manually, and also
# add the kernel cmdline options that virtme-init uses to setup the interface.
readonly QEMU_TEST_PORT_FWD="hostfwd=tcp::${TEST_HOST_PORT}-:${TEST_GUEST_PORT}"
readonly QEMU_SSH_PORT_FWD="hostfwd=tcp::${SSH_HOST_PORT}-:${SSH_GUEST_PORT}"
readonly KERNEL_CMDLINE="\
	virtme.dhcp net.ifnames=0 biosdevname=0 \
	virtme.ssh virtme_ssh_channel=tcp virtme_ssh_user=$USER \
"
readonly LOG=$(mktemp /tmp/vsock_vmtest_XXXX.log)
readonly TEST_NAMES=(
	vm_server_host_client
	vm_client_host_server
	vm_loopback
	host_vsock_ns_mode_ok
	host_vsock_ns_mode_write_once_ok
	global_same_cid_fails
	local_same_cid_ok
	global_local_same_cid_ok
	local_global_same_cid_ok
	diff_ns_global_host_connect_to_global_vm_ok
	diff_ns_global_host_connect_to_local_vm_fails
	diff_ns_global_vm_connect_to_global_host_ok
	diff_ns_global_vm_connect_to_local_host_fails
	diff_ns_local_host_connect_to_local_vm_fails
	diff_ns_local_vm_connect_to_local_host_fails
	diff_ns_global_to_local_loopback_local_fails
	diff_ns_local_to_global_loopback_fails
	diff_ns_local_to_local_loopback_fails
	diff_ns_global_to_global_loopback_ok
	same_ns_local_loopback_ok
	same_ns_local_host_connect_to_local_vm_ok
	same_ns_local_vm_connect_to_local_host_ok
)

readonly TEST_DESCS=(
	# vm_server_host_client
	"Run vsock_test in server mode on the VM and in client mode on the host."

	# vm_client_host_server
	"Run vsock_test in client mode on the VM and in server mode on the host."

	# vm_loopback
	"Run vsock_test using the loopback transport in the VM."

	# host_vsock_ns_mode_ok
	"Check /proc/net/vsock_ns_mode strings on the host."

	# host_vsock_ns_mode_write_once_ok
	"Check /proc/net/vsock_ns_mode is write-once on the host."

	# global_same_cid_fails
	"Check QEMU fails to start two VMs with same CID in two different global namespaces."

	# local_same_cid_ok
	"Check QEMU successfully starts two VMs with same CID in two different local namespaces."

	# global_local_same_cid_ok
	"Check QEMU successfully starts one VM in a global ns and then another VM in a local ns with the same CID."

	# local_global_same_cid_ok
	"Check QEMU successfully starts one VM in a local ns and then another VM in a global ns with the same CID."

	# diff_ns_global_host_connect_to_global_vm_ok
	"Run vsock_test client in global ns with server in VM in another global ns."

	# diff_ns_global_host_connect_to_local_vm_fails
	"Run socat to test a process in a global ns fails to connect to a VM in a local ns."

	# diff_ns_global_vm_connect_to_global_host_ok
	"Run vsock_test client in VM in a global ns with server in another global ns."

	# diff_ns_global_vm_connect_to_local_host_fails
	"Run socat to test a VM in a global ns fails to connect to a host process in a local ns."

	# diff_ns_local_host_connect_to_local_vm_fails
	"Run socat to test a host process in a local ns fails to connect to a VM in another local ns."

	# diff_ns_local_vm_connect_to_local_host_fails
	"Run socat to test a VM in a local ns fails to connect to a host process in another local ns."

	# diff_ns_global_to_local_loopback_local_fails
	"Run socat to test a loopback vsock in a global ns fails to connect to a vsock in a local ns."

	# diff_ns_local_to_global_loopback_fails
	"Run socat to test a loopback vsock in a local ns fails to connect to a vsock in a global ns."

	# diff_ns_local_to_local_loopback_fails
	"Run socat to test a loopback vsock in a local ns fails to connect to a vsock in another local ns."

	# diff_ns_global_to_global_loopback_ok
	"Run socat to test a loopback vsock in a global ns successfuly connects to a vsock in another global ns."

	# same_ns_local_loopback_ok
	"Run socat to test a loopback vsock in a local ns successfuly connects to a vsock in the same ns."

	# same_ns_local_host_connect_to_local_vm_ok
	"Run vsock_test client in a local ns with server in VM in same ns."

	# same_ns_local_vm_connect_to_local_host_ok
	"Run vsock_test client in VM in a local ns with server in same ns."
)

readonly USE_SHARED_VM=(vm_server_host_client vm_client_host_server vm_loopback)
readonly USE_INIT_NETNS=(
	global_same_cid_fails
	local_same_cid_ok
	global_local_same_cid_ok
	local_global_same_cid_ok
	diff_ns_global_host_connect_to_global_vm_ok
	diff_ns_global_host_connect_to_local_vm_fails
	diff_ns_global_vm_connect_to_global_host_ok
	diff_ns_global_vm_connect_to_local_host_fails
	diff_ns_local_host_connect_to_local_vm_fails
	diff_ns_local_vm_connect_to_local_host_fails
	diff_ns_global_to_local_loopback_local_fails
	diff_ns_local_to_global_loopback_fails
	diff_ns_local_to_local_loopback_fails
	diff_ns_global_to_global_loopback_ok
	same_ns_local_loopback_ok
	same_ns_local_host_connect_to_local_vm_ok
	same_ns_local_vm_connect_to_local_host_ok
)
readonly MODES=("local" "global")

readonly LOG_LEVEL_DEBUG=0
readonly LOG_LEVEL_INFO=1
readonly LOG_LEVEL_WARN=2
readonly LOG_LEVEL_ERROR=3

VERBOSE="${LOG_LEVEL_WARN}"

# Test pass/fail counters
cnt_pass=0
cnt_fail=0
cnt_skip=0
cnt_total=0

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
		printf "\t%-55s%-35s\n" "${name}" "${desc}"
	done
	echo

	exit 1
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
}

add_namespaces() {
	# add namespaces local0, local1, global0, and global1
	for mode in "${MODES[@]}"; do
		ip netns add "${mode}0" 2>/dev/null
		ip netns add "${mode}1" 2>/dev/null
	done
}

init_namespaces() {
	for mode in "${MODES[@]}"; do
		ns_set_mode "${mode}0" "${mode}"
		ns_set_mode "${mode}1" "${mode}"

		log_host "set ns ${mode}0 to mode ${mode}"
		log_host "set ns ${mode}1 to mode ${mode}"

		# we need lo for qemu port forwarding
		ip netns exec "${mode}0" ip link set dev lo up
		ip netns exec "${mode}1" ip link set dev lo up
	done
}

del_namespaces() {
	for mode in "${MODES[@]}"; do
		ip netns del "${mode}0"
		ip netns del "${mode}1"
		log_host "removed ns ${mode}0"
		log_host "removed ns ${mode}1"
	done &>/dev/null
}

ns_set_mode() {
	local ns=$1
	local mode=$2

	echo "${mode}" | ip netns exec "${ns}" \
		tee /proc/net/vsock_ns_mode &>/dev/null
}

vm_ssh() {
	local ns_exec

	if [[ "${1}" == none ]]; then
		local ns_exec=""
	else
		local ns_exec="ip netns exec ${1}"
	fi

	shift

	${ns_exec} ssh -q -o UserKnownHostsFile=/dev/null -p ${SSH_HOST_PORT} localhost $*

	return $?
}

cleanup() {
	del_namespaces
}

terminate_pidfiles() {
	local pidfile

	for pidfile in "$@"; do
		if [[ -s "${pidfile}" ]]; then
			pkill -SIGTERM -F "${pidfile}" 2>&1 > /dev/null
		fi

		# If failure occurred during or before qemu start up, then we need
		# to clean this up ourselves.
		if [[ -e "${pidfile}" ]]; then
			rm -f "${pidfile}"
		fi
	done
}

terminate_pids() {
	local pid

	for pid in "$@"; do
		kill -SIGTERM "${pid}" &>/dev/null || :
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
	for dep in vng ${QEMU} busybox pkill ssh socat; do
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

check_socat() {
	local support_string

	support_string="$(socat -V)"

	if [[ "${support_string}" != *"WITH_VSOCK 1"* ]]; then
		die "err: socat is missing vsock support"
	fi

	if [[ "${support_string}" != *"WITH_UNIX 1"* ]]; then
		die "err: socat is missing unix support"
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
	local pidfile=$3
	local logfile=/dev/null
	local verbose_opt=""
	local qemu_opts=""
	local kernel_opt=""
	local ns_exec=""
	local qemu

	qemu=$(command -v "${QEMU}")

	if [[ ${VERBOSE} -le ${LOG_LEVEL_DEBUG} ]]; then
		verbose_opt="--verbose"
		logfile=/dev/stdout
	fi

	qemu_opts="\
		 -netdev user,id=n0,${QEMU_TEST_PORT_FWD},${QEMU_SSH_PORT_FWD} \
		 -device virtio-net-pci,netdev=n0 \
		${QEMU_OPTS} -device vhost-vsock-pci,guest-cid=${cid} \
		--pidfile ${pidfile}
	"

	if [[ "${BUILD}" -eq 1 ]]; then
		kernel_opt="${KERNEL_CHECKOUT}"
	fi

	if [[ "${ns}" != "none" ]]; then
		ns_exec="ip netns exec ${ns}"
	fi

	${ns_exec} vng \
		--run \
		${kernel_opt} \
		${verbose_opt} \
		--qemu-opts="${qemu_opts}" \
		--qemu="${qemu}" \
		--user root \
		--append "${KERNEL_CMDLINE}" \
		--rw  &> ${logfile} &

	timeout "${WAIT_QEMU}" \
		bash -c 'while [[ ! -s '"${pidfile}"' ]]; do sleep 1; done; exit 0'
}

vm_wait_for_ssh() {
	local ns=$1
	local i

	i=0
	while true; do
		if [[ ${i} -gt ${WAIT_PERIOD_MAX} ]]; then
			die "Timed out waiting for guest ssh"
		fi

		if vm_ssh "${ns}" -- true; then
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
	local old_pipefail
	local protocol=tcp
	local pattern
	local i

	pattern=":$(printf "%04X" "${port}") "

	# for tcp protocol additionally check the socket state
	[ "${protocol}" = "tcp" ] && pattern="${pattern}0A"

	# 'grep -q' exits on match, sending SIGPIPE to 'awk', which exits with
	# an error, causing the if-condition to fail when pipefail is set.
	# Instead, temporarily disable pipefail and restore it later.
	old_pipefail=$(set -o | awk '/^pipefail[[:space:]]+(on|off)$/{print $2}')
	set +o pipefail

	for i in $(seq "${max_intervals}"); do
		if awk '{print $2" "$4}' /proc/net/"${protocol}"* | \
		   grep -q "${pattern}"; then
			break
		fi

		sleep "${interval}"
	done

	if [[ "${old_pipefail}" == on ]]; then
		set -o pipefail
	fi
}

vm_wait_for_listener() {
	local ns=$1
	local port=$2

	log "Waiting for listener on port ${port} on vm"

	vm_ssh "${ns}" <<EOF
$(declare -f wait_for_listener)
wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
EOF
}

host_wait_for_listener() {
	local ns=$1
	local port=$2

	if [[ "${ns}" == none ]]; then
		wait_for_listener "${port}" "${WAIT_PERIOD}" "${WAIT_PERIOD_MAX}"
	else
		ip netns exec "${ns}" bash <<-EOF
			$(declare -f wait_for_listener)
			wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
		EOF
	fi
}

log() {
	local redirect
	local prefix

	if [[ ${VERBOSE} -gt ${LOG_LEVEL_INFO} ]]; then
		redirect=/dev/null
	else
		redirect=/dev/stdout
	fi

	prefix="${LOG_PREFIX:-}"

	if [[ "$#" -eq 0 ]]; then
		if [[ -n "${prefix}" ]]; then
			cat | awk -v prefix="${prefix}" '{printf "%s: %s\n", prefix, $0}'
		else
			cat
		fi
	else
		if [[ -n "${prefix}" ]]; then
			echo "${prefix}: " "$@"
		else
			echo "$@"
		fi
	fi | tee -a "${LOG}" > ${redirect}
}

log_host() {
	LOG_PREFIX=host log $@
}

log_guest() {
	LOG_PREFIX=guest log $@
}

vm_vsock_test() {
	local ns=$1
	local mode=$2
	local rc

	set -o pipefail
	if [[ "${mode}" == client ]]; then
		local host=$3
		local cid=$4
		local port=$5

		# log output and use pipefail to respect vsock_test errors
		vm_ssh "${ns}" -- "${VSOCK_TEST}" \
			--mode=client \
			--control-host="${host}" \
			--peer-cid="${cid}" \
			--control-port="${port}" \
			2>&1 | log_guest
		rc=$?
	else
		local cid=$3
		local port=$4

		# log output and use pipefail to respect vsock_test errors
		vm_ssh "${ns}" -- "${VSOCK_TEST}" \
			--mode=server \
			--peer-cid="${cid}" \
			--control-port="${port}" \
			2>&1 | log_guest &
		rc=$?

		if [[ $rc -ne 0 ]]; then
			set +o pipefail
			return $rc
		fi

		vm_wait_for_listener "${ns}" "${port}"
		rc=$?
	fi
	set +o pipefail

	return $rc
}

host_vsock_test() {
	local ns=$1
	local mode=$2
	local cmd

	if [[ "${ns}" == none ]]; then
		cmd="${VSOCK_TEST}"
	else
		cmd="ip netns exec ${ns} ${VSOCK_TEST}"
	fi

	# log output and use pipefail to respect vsock_test errors
	set -o pipefail
	if [[ "${mode}" == client ]]; then
		local host=$3
		local cid=$4
		local port=$5

		${cmd} \
			--mode="${mode}" \
			--peer-cid="${cid}" \
			--control-host="${host}" \
			--control-port="${port}" 2>&1 | log_host
		rc=$?
	else
		local cid=$3
		local port=$4

		${cmd} \
			--mode="${mode}" \
			--peer-cid="${cid}" \
			--control-port="${port}" 2>&1 | log_host &
		rc=$?

		if [[ $rc -ne 0 ]]; then
			return $rc
		fi

		host_wait_for_listener "${ns}" "${port}" "${WAIT_PERIOD}" "${WAIT_PERIOD_MAX}"
		rc=$?
	fi
	set +o pipefail

	return $rc
}

test_vm_server_host_client() {
	vm_vsock_test "none" "server" 2 "${TEST_GUEST_PORT}"
	host_vsock_test "none" "client" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
}

test_vm_client_host_server() {
	host_vsock_test "none" "server" "${VSOCK_CID}" "${TEST_HOST_PORT_LISTENER}"
	vm_vsock_test "none" "client" "10.0.2.2" 2 "${TEST_HOST_PORT_LISTENER}"
}

test_vm_loopback() {
	vm_vsock_test "none" "server" 1 "${TEST_HOST_PORT_LISTENER}"
	vm_vsock_test "none" "client" "127.0.0.1" 1 "${TEST_HOST_PORT_LISTENER}"
}

test_host_vsock_ns_mode_ok() {
	add_namespaces

	for mode in "${MODES[@]}"; do
		if ! ns_set_mode "${mode}0" "${mode}"; then
			del_namespaces
			return "${KSFT_FAIL}"
		fi
	done

	del_namespaces
}

test_host_vsock_ns_mode_write_once_ok() {
	add_namespaces

	for mode in "${MODES[@]}"; do
		local ns="${mode}0"
		if ! ns_set_mode "${ns}" "${mode}"; then
			del_namespaces
			return "${KSFT_FAIL}"
		fi

		# try writing again and expect failure
		if ns_set_mode "${ns}" "${mode}"; then
			del_namespaces
			return "${KSFT_FAIL}"
		fi
	done

	del_namespaces

	return "${KSFT_PASS}"
}

namespaces_can_boot_same_cid() {
	local ns0=$1
	local ns1=$2
	local pidfile1 pidfile2
	local cid=20
	readonly cid
	local rc

	pidfile1=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	vm_start "${cid}" "${ns0}" "${pidfile1}"

	pidfile2=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	vm_start "${cid}" "${ns1}" "${pidfile2}"

	rc=$?
	terminate_pidfiles "${pidfile1}" "${pidfile2}"

	return $rc
}

test_global_same_cid_fails() {
	if namespaces_can_boot_same_cid "global0" "global1"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_local_global_same_cid_ok() {
	if namespaces_can_boot_same_cid "local0" "global0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_global_local_same_cid_ok() {
	if namespaces_can_boot_same_cid "global0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_local_same_cid_ok() {
	if namespaces_can_boot_same_cid "local0" "local0"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_diff_ns_global_host_connect_to_global_vm_ok() {
	local pids pid pidfile
	local ns0 ns1 port
	declare -a pids
	local unixfile
	ns0="global0"
	ns1="global1"
	port=1234
	local rc

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)

	if ! vm_start "${VSOCK_CID}" "${ns0}" "${pidfile}"; then
		return "${KSFT_FAIL}"
	fi

	unixfile=$(mktemp -u /tmp/XXXX.sock)
	ip netns exec "${ns1}" \
		socat TCP-LISTEN:"${TEST_HOST_PORT}",fork \
			UNIX-CONNECT:"${unixfile}" &
	pids+=($!)
	host_wait_for_listener "${ns1}" "${TEST_HOST_PORT}"

	ip netns exec "${ns0}" socat UNIX-LISTEN:"${unixfile}",fork \
		TCP-CONNECT:localhost:"${TEST_HOST_PORT}" &
	pids+=($!)

	vm_vsock_test "${ns0}" "server" 2 "${TEST_GUEST_PORT}"
	vm_wait_for_listener "${ns0}" "${TEST_GUEST_PORT}"
	host_vsock_test "${ns1}" "client" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
	rc=$?

	for pid in "${pids[@]}"; do
		if [[ "$(jobs -p)" = *"${pid}"* ]]; then
			kill -SIGTERM "${pid}" &>/dev/null
		fi
	done

	terminate_pidfiles "${pidfile}"

	if [[ $rc -ne 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_diff_ns_global_host_connect_to_local_vm_fails() {
	local ns0="global0"
	local ns1="local0"
	local port=12345
	local pidfile
	local result
	local pid

	outfile=$(mktemp)

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	if ! vm_start "${VSOCK_CID}" "${ns1}" "${pidfile}"; then
		log_host "failed to start vm (cid=${VSOCK_CID}, ns=${ns0})"
		return $KSFT_FAIL
	fi

	vm_wait_for_ssh "${ns1}"
	vm_ssh "${ns1}" -- socat VSOCK-LISTEN:"${port}" STDOUT > "${outfile}" &
	echo TEST | ip netns exec "${ns0}" \
		socat STDIN VSOCK-CONNECT:"${VSOCK_CID}":"${port}" 2>/dev/null

	terminate_pidfiles "${pidfile}"

	result=$(cat "${outfile}")
	rm -f "${outfile}"

	if [[ "${result}" != TEST ]]; then
		return $KSFT_PASS
	fi

	return $KSFT_FAIL
}

test_diff_ns_global_vm_connect_to_global_host_ok() {
	local ns0="global0"
	local ns1="global1"
	local port=12345
	local unixfile
	local pidfile
	local pids

	declare -a pids

	log_host "Setup socat bridge from ns ${ns0} to ns ${ns1} over port ${port}"

	unixfile=$(mktemp -u /tmp/XXXX.sock)

	ip netns exec "${ns0}" \
		socat TCP-LISTEN:"${port}" UNIX-CONNECT:"${unixfile}" &
	pids+=($!)

	ip netns exec "${ns1}" \
		socat UNIX-LISTEN:"${unixfile}" TCP-CONNECT:127.0.0.1:"${port}" &
	pids+=($!)

	log_host "Launching ${VSOCK_TEST} in ns ${ns1}"
	host_vsock_test "${ns1}" "server" "${VSOCK_CID}" "${port}"

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	if ! vm_start "${VSOCK_CID}" "${ns0}" "${pidfile}"; then
		log_host "failed to start vm (cid=${cid}, ns=${ns0})"
		terminate_pids "${pids[@]}"
		rm -f "${unixfile}"
		return $KSFT_FAIL
	fi

	vm_wait_for_ssh "${ns0}"
	vm_vsock_test "${ns0}" "client" "10.0.2.2" 2 "${port}"
	rc=$?

	terminate_pidfiles "${pidfile}"
	terminate_pids "${pids[@]}"
	rm -f "${unixfile}"

	if [[ ! $rc -eq 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"

}

test_diff_ns_global_vm_connect_to_local_host_fails() {
	local ns0="global0"
	local ns1="local0"
	local port=12345
	local pidfile
	local result
	local pid

	log_host "Launching socat in ns ${ns1}"
	outfile=$(mktemp)
	ip netns exec "${ns1}" socat VSOCK-LISTEN:${port} STDOUT &> "${outfile}" &
	pid=$!

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	if ! vm_start "${VSOCK_CID}" "${ns0}" "${pidfile}"; then
		log_host "failed to start vm (cid=${cid}, ns=${ns0})"
		terminate_pids "${pid}"
		rm -f "${outfile}"
		return $KSFT_FAIL
	fi

	vm_wait_for_ssh "${ns0}"

	vm_ssh "${ns0}" -- \
		bash -c "echo TEST | socat STDIN VSOCK-CONNECT:2:${port}" 2>&1 | log_guest

	terminate_pidfiles "${pidfile}"
	terminate_pids "${pid}"

	result=$(cat "${outfile}")
	rm -f "${outfile}"

	if [[ "${result}" != TEST ]]; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_diff_ns_local_host_connect_to_local_vm_fails() {
	local ns0="local0"
	local ns1="local1"
	local port=12345
	local pidfile
	local result
	local pid

	outfile=$(mktemp)

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	if ! vm_start "${VSOCK_CID}" "${ns1}" "${pidfile}"; then
		log_host "failed to start vm (cid=${cid}, ns=${ns0})"
		return $KSFT_FAIL
	fi

	vm_wait_for_ssh "${ns1}"
	vm_ssh "${ns1}" -- socat VSOCK-LISTEN:"${port}" STDOUT > "${outfile}" &
	echo TEST | ip netns exec "${ns0}" \
		socat STDIN VSOCK-CONNECT:"${VSOCK_CID}":"${port}" 2>/dev/null

	terminate_pidfiles "${pidfile}"

	result=$(cat "${outfile}")
	rm -f "${outfile}"

	if [[ "${result}" != TEST ]]; then
		return $KSFT_PASS
	fi

	return $KSFT_FAIL
}

test_diff_ns_local_vm_connect_to_local_host_fails() {
	local ns0="local0"
	local ns1="local1"
	local port=12345
	local pidfile
	local result
	local pid

	log_host "Launching socat in ns ${ns1}"
	outfile=$(mktemp)
	ip netns exec "${ns1}" socat VSOCK-LISTEN:"${port}" STDOUT &> "${outfile}" &
	pid=$!

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)
	if ! vm_start "${VSOCK_CID}" "${ns0}" "${pidfile}"; then
		log_host "failed to start vm (cid=${cid}, ns=${ns0})"
		rm -f "${outfile}"
		return "${KSFT_FAIL}"
	fi

	vm_wait_for_ssh "${ns0}"

	vm_ssh "${ns0}" -- \
		bash -c "echo TEST | socat STDIN VSOCK-CONNECT:2:${port}" 2>&1 | log_guest

	terminate_pidfiles "${pidfile}"
	terminate_pids "${pid}"

	result=$(cat "${outfile}")
	rm -f "${outfile}"

	if [[ "${result}" != TEST ]]; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

__test_loopback_two_netns() {
	local ns0=$1
	local ns1=$2
	local port=12345
	local result
	local pid

	log_host "Launching socat in ns ${ns1}"
	outfile=$(mktemp)
	ip netns exec "${ns1}" socat VSOCK-LISTEN:"${port}" STDOUT > "${outfile}" 2>/dev/null &
	pid=$!

	log_host "Launching socat in ns ${ns0}"
	echo TEST | ip netns exec "${ns0}" socat STDIN VSOCK-CONNECT:1:"${port}" 2>/dev/null
	terminate_pids "${pid}"

	result=$(cat "${outfile}")
	rm -f "${outfile}"

	if [[ "${result}" == TEST ]]; then
		return 0
	fi

	return 1
}

test_diff_ns_global_to_local_loopback_local_fails() {
	if ! __test_loopback_two_netns "global0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_diff_ns_local_to_global_loopback_fails() {
	if ! __test_loopback_two_netns "local0" "global0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_diff_ns_local_to_local_loopback_fails() {
	if ! __test_loopback_two_netns "local0" "local1"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_diff_ns_global_to_global_loopback_ok() {
	if __test_loopback_two_netns "global0" "global1"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_same_ns_local_loopback_ok() {
	if __test_loopback_two_netns "local0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_same_ns_local_host_connect_to_local_vm_ok() {
	local ns="local0"
	local port=1234
	local pidfile
	local rc

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)

	if ! vm_start "${VSOCK_CID}" "${ns}" "${pidfile}"; then
		return "${KSFT_FAIL}"
	fi

	vm_vsock_test "${ns}" "server" 2 "${TEST_GUEST_PORT}"
	host_vsock_test "${ns}" "client" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
	rc=$?

	terminate_pidfiles "${pidfile}"

	if [[ $rc -ne 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_same_ns_local_vm_connect_to_local_host_ok() {
	local ns="local0"
	local port=1234
	local pidfile
	local rc

	pidfile=$(mktemp /tmp/qemu_vsock_vmtest_XXXX.pid)

	if ! vm_start "${VSOCK_CID}" "${ns}" "${pidfile}"; then
		return "${KSFT_FAIL}"
	fi

	vm_vsock_test "${ns}" "server" 2 "${TEST_GUEST_PORT}"
	host_vsock_test "${ns}" "client" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
	rc=$?

	terminate_pidfiles "${pidfile}"

	if [[ $rc -ne 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

shared_vm_test() {
	local tname

	tname="${1}"

	for testname in "${USE_SHARED_VM[@]}"; do
		if [[ "${tname}" == "${testname}" ]]; then
			return 0
		fi
	done

	return 1
}


init_netns_test() {
	local tname

	tname="${1}"

	for testname in "${USE_INIT_NETNS[@]}"; do
		if [[ "${tname}" == "${testname}" ]]; then
			return 0
		fi
	done

	return 1
}

check_result() {
	local rc num

	rc=$1
	num=$(( cnt_total + 1 ))

	if [[ ${rc} -eq $KSFT_PASS ]]; then
		cnt_pass=$(( cnt_pass + 1 ))
		echo "ok ${num} ${arg}"
	elif [[ ${rc} -eq $KSFT_SKIP ]]; then
		cnt_skip=$(( cnt_skip + 1 ))
		echo "ok ${num} ${arg} # SKIP"
	elif [[ ${rc} -eq $KSFT_FAIL ]]; then
		cnt_fail=$(( cnt_fail + 1 ))
		echo "not ok ${num} ${arg} # exit=$rc"
	fi

	cnt_total=$(( cnt_total + 1 ))
}

run_shared_vm_tests() {
	local start_shared_vm pidfile
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

	start_shared_vm=0

	for arg in "${ARGS[@]}"; do
		if shared_vm_test "${arg}"; then
			start_shared_vm=1
			break
		fi
	done

	pidfile=""
	if [[ "${start_shared_vm}" == 1 ]]; then
		pidfile=$(mktemp $PIDFILE_TEMPLATE)
		log_host "Booting up VM"
		vm_start "${VSOCK_CID}" "none" "${pidfile}"
		vm_wait_for_ssh "none"
		log_host "VM booted up"
	fi

	for arg in "${ARGS[@]}"; do
		if ! shared_vm_test "${arg}"; then
			continue
		fi

		host_oops_cnt_before=$(dmesg | grep -c -i 'Oops')
		host_warn_cnt_before=$(dmesg --level=warn | wc -l)
		vm_oops_cnt_before=$(vm_ssh none -- dmesg | grep -c -i 'Oops')
		vm_warn_cnt_before=$(vm_ssh none -- dmesg --level=warn | wc -l)

		name=$(echo "${arg}" | awk '{ print $1 }')
		log_host "Executing test_${name}"
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

		vm_oops_cnt_after=$(vm_ssh none -- dmesg | grep -i 'Oops' | wc -l)
		if [[ ${vm_oops_cnt_after} -gt ${vm_oops_cnt_before} ]]; then
			echo "FAIL: kernel oops detected on vm" | log_host "${name}"
			rc=$KSFT_FAIL
		fi

		vm_warn_cnt_after=$(vm_ssh none -- dmesg --level=warn | wc -l)
		if [[ ${vm_warn_cnt_after} -gt ${vm_warn_cnt_before} ]]; then
			echo "FAIL: kernel warning detected on vm" | log_host "${name}"
			rc=$KSFT_FAIL
		fi

		check_result "${rc}"
	done

	if [[ -n "${pidfile}" ]]; then
		log_host "VM terminate"
		terminate_pidfiles "${pidfile}"
	fi
}

run_isolated_vm_tests() {
	for arg in "${ARGS[@]}"; do
		if shared_vm_test "${arg}"; then
			continue
		fi

		add_namespaces
		if init_netns_test "${arg}"; then
			init_namespaces
		fi

		name=$(echo "${arg}" | awk '{ print $1 }')
		log_host "Executing test_${name}"
		eval test_"${name}"
		check_result $?

		del_namespaces
	done
}

QEMU="qemu-system-$(uname -m)"

while getopts :hvsq:b o
do
	case $o in
	v) VERBOSE=$(( VERBOSE - 1 ));;
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
check_socat
handle_build

echo "1..${#ARGS[@]}"

run_shared_vm_tests
run_isolated_vm_tests

echo "SUMMARY: PASS=${cnt_pass} SKIP=${cnt_skip} FAIL=${cnt_fail}"
echo "Log: ${LOG}"

if [ $((cnt_pass + cnt_skip)) -eq ${cnt_total} ]; then
	exit "$KSFT_PASS"
else
	exit "$KSFT_FAIL"
fi
