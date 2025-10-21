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
readonly WAIT_QEMU=5
readonly PIDFILE_TEMPLATE=/tmp/vsock_vmtest_XXXX.pid

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
	ns_host_vsock_ns_mode_ok
	ns_host_vsock_ns_mode_write_once_ok
	ns_global_same_cid_fails
	ns_local_same_cid_ok
	ns_global_local_same_cid_ok
	ns_local_global_same_cid_ok
	ns_diff_global_host_connect_to_global_vm_ok
	ns_diff_global_host_connect_to_local_vm_fails
	ns_diff_global_vm_connect_to_global_host_ok
	ns_diff_global_vm_connect_to_local_host_fails
	ns_diff_local_host_connect_to_local_vm_fails
	ns_diff_local_vm_connect_to_local_host_fails
	ns_diff_global_to_local_loopback_local_fails
	ns_diff_local_to_global_loopback_fails
	ns_diff_local_to_local_loopback_fails
	ns_diff_global_to_global_loopback_ok
	ns_same_local_loopback_ok
	ns_same_local_host_connect_to_local_vm_ok
	ns_same_local_vm_connect_to_local_host_ok
	ns_mode_change_connection_continue_vm_ok
	ns_mode_change_connection_continue_host_ok
	ns_mode_change_connection_continue_both_ok
	ns_delete_vm_ok
	ns_delete_host_ok
	ns_delete_both_ok
	ns_loopback_global_global_late_module_load_ok
	ns_loopback_local_local_late_module_load_fails
)
readonly TEST_DESCS=(
	# vm_server_host_client
	"Run vsock_test in server mode on the VM and in client mode on the host."

	# vm_client_host_server
	"Run vsock_test in client mode on the VM and in server mode on the host."

	# vm_loopback
	"Run vsock_test using the loopback transport in the VM."

	# ns_host_vsock_ns_mode_ok
	"Check /proc/sys/net/vsock/ns_mode strings on the host."

	# ns_host_vsock_ns_mode_write_once_ok
	"Check /proc/sys/net/vsock/ns_mode is write-once on the host."

	# ns_global_same_cid_fails
	"Check QEMU fails to start two VMs with same CID in two different global namespaces."

	# ns_local_same_cid_ok
	"Check QEMU successfully starts two VMs with same CID in two different local namespaces."

	# ns_global_local_same_cid_ok
	"Check QEMU successfully starts one VM in a global ns and then another VM in a local ns with the same CID."

	# ns_local_global_same_cid_ok
	"Check QEMU successfully starts one VM in a local ns and then another VM in a global ns with the same CID."

	# ns_diff_global_host_connect_to_global_vm_ok
	"Run vsock_test client in global ns with server in VM in another global ns."

	# ns_diff_global_host_connect_to_local_vm_fails
	"Run socat to test a process in a global ns fails to connect to a VM in a local ns."

	# ns_diff_global_vm_connect_to_global_host_ok
	"Run vsock_test client in VM in a global ns with server in another global ns."

	# ns_diff_global_vm_connect_to_local_host_fails
	"Run socat to test a VM in a global ns fails to connect to a host process in a local ns."

	# ns_diff_local_host_connect_to_local_vm_fails
	"Run socat to test a host process in a local ns fails to connect to a VM in another local ns."

	# ns_diff_local_vm_connect_to_local_host_fails
	"Run socat to test a VM in a local ns fails to connect to a host process in another local ns."

	# ns_diff_global_to_local_loopback_local_fails
	"Run socat to test a loopback vsock in a global ns fails to connect to a vsock in a local ns."

	# ns_diff_local_to_global_loopback_fails
	"Run socat to test a loopback vsock in a local ns fails to connect to a vsock in a global ns."

	# ns_diff_local_to_local_loopback_fails
	"Run socat to test a loopback vsock in a local ns fails to connect to a vsock in another local ns."

	# ns_diff_global_to_global_loopback_ok
	"Run socat to test a loopback vsock in a global ns successfully connects to a vsock in another global ns."

	# ns_same_local_loopback_ok
	"Run socat to test a loopback vsock in a local ns successfully connects to a vsock in the same ns."

	# ns_same_local_host_connect_to_local_vm_ok
	"Run vsock_test client in a local ns with server in VM in same ns."

	# ns_same_local_vm_connect_to_local_host_ok
	"Run vsock_test client in VM in a local ns with server in same ns."

	# ns_mode_change_connection_continue_vm_ok
	"Check that changing NS mode of VM namespace from global to local after a connection is established doesn't break the connection"

	# ns_mode_change_connection_continue_host_ok
	"Check that changing NS mode of host namespace from global to local after a connection is established doesn't break the connection"

	# ns_mode_change_connection_continue_both_ok
	"Check that changing NS mode of host and VM namespaces from global to local after a connection is established doesn't break the connection"

	# ns_delete_vm_ok
	"Check that deleting the VM's namespace does not break the socket connection"

	# ns_delete_host_ok
	"Check that deleting the host's namespace does not break the socket connection"

	# ns_delete_both_ok
	"Check that deleting the VM and host's namespaces does not break the socket connection"

	# ns_loopback_global_global_late_module_load_ok
	"Test that loopback still works in global namespaces initialized prior to loading the vsock_loopback kmod"

	# ns_loopback_local_local_late_module_load_fails
	"Test that loopback connections still fail between local namespaces initialized prior to loading the vsock_loopback kmod"
)

readonly USE_SHARED_VM=(vm_server_host_client vm_client_host_server vm_loopback)
readonly NS_MODES=("local" "global")

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
		printf "\t%-55s%-35s\n" "${name}" "${desc}"
	done
	echo

	exit 1
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
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

add_namespaces() {
	# add namespaces local0, local1, global0, and global1
	for mode in "${NS_MODES[@]}"; do
		ip netns add "${mode}0" 2>/dev/null
		ip netns add "${mode}1" 2>/dev/null
	done
}

init_namespaces() {
	for mode in "${NS_MODES[@]}"; do
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
	for mode in "${NS_MODES[@]}"; do
		ip netns del "${mode}0" &>/dev/null
		ip netns del "${mode}1" &>/dev/null
		log_host "removed ns ${mode}0"
		log_host "removed ns ${mode}1"
	done
}

ns_set_mode() {
	local ns=$1
	local mode=$2

	echo "${mode}" | ip netns exec "${ns}" \
		tee /proc/sys/net/vsock/ns_mode &>/dev/null
}

vm_ssh() {
	local ns_exec

	if [[ "${1}" == init_ns ]]; then
		ns_exec=""
	else
		ns_exec="ip netns exec ${1}"
	fi

	shift

	${ns_exec} ssh -q -o UserKnownHostsFile=/dev/null -p ${SSH_HOST_PORT} localhost $*

	return $?
}

cleanup() {
	del_namespaces
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

check_netns() {
	local tname=$1

	# If the test requires NS support, check if NS support exists
	# using /proc/self/ns
	if [[ "${tname}" =~ ^ns_ ]] &&
	   [[ ! -e /proc/self/ns ]]; then
		log_host "No NS support detected for test ${tname}"
		return 1
	fi

	return 0
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

terminate_pidfiles() {
	local pidfile

	for pidfile in "$@"; do
		if [[ -s "${pidfile}" ]]; then
			pkill -SIGTERM -F "${pidfile}" > /dev/null 2>&1
		fi

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

vm_start() {
	local pidfile=$1
	local ns=$2
	local logfile=/dev/null
	local verbose_opt=""
	local kernel_opt=""
	local qemu_opts=""
	local ns_exec=""
	local qemu

	qemu=$(command -v "${QEMU}")

	if [[ "${VERBOSE}" -eq 1 ]]; then
		verbose_opt="--verbose"
		logfile=/dev/stdout
	fi

	qemu_opts="\
		 -netdev user,id=n0,${QEMU_TEST_PORT_FWD},${QEMU_SSH_PORT_FWD} \
		 -device virtio-net-pci,netdev=n0 \
		 -device vhost-vsock-pci,guest-cid=${VSOCK_CID} \
		--pidfile ${pidfile}
	"

	if [[ "${BUILD}" -eq 1 ]]; then
		kernel_opt="${KERNEL_CHECKOUT}"
	fi

	if [[ "${ns}" != "init_ns" ]]; then
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

	if [[ "${ns}" == init_ns ]]; then
		wait_for_listener "${port}" "${WAIT_PERIOD}" "${WAIT_PERIOD_MAX}"
	else
		ip netns exec "${ns}" bash <<-EOF
			$(declare -f wait_for_listener)
			wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
		EOF
	fi
}

vm_vsock_test() {
	local ns=$1
	local host=$2
	local cid=$3
	local port=$4
	local rc

	set -o pipefail
	if [[ "${host}" != server ]]; then
		# log output and use pipefail to respect vsock_test errors
		vm_ssh "${ns}" -- "${VSOCK_TEST}" \
			--mode=client \
			--control-host="${host}" \
			--peer-cid="${cid}" \
			--control-port="${port}" \
			2>&1 | log_guest
		rc=$?
	else
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
	local host=$2
	local cid=$3
	local port=$4
	local rc

	local cmd="${VSOCK_TEST}"
	if [[ "${ns}" != "init_ns" ]]; then
		cmd="ip netns exec ${ns} ${cmd}"
	fi

	# log output and use pipefail to respect vsock_test errors
	set -o pipefail
	if [[ "${host}" != server ]]; then
		${cmd} \
			--mode=client \
			--peer-cid="${cid}" \
			--control-host="${host}" \
			--control-port="${port}" 2>&1 | log_host
		rc=$?
	else
		${cmd} \
			--mode=server \
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

log() {
	local redirect
	local prefix

	if [[ ${VERBOSE} -eq 0 ]]; then
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

test_ns_host_vsock_ns_mode_ok() {
	add_namespaces

	for mode in "${NS_MODES[@]}"; do
		if ! ns_set_mode "${mode}0" "${mode}"; then
			del_namespaces
			return "${KSFT_FAIL}"
		fi
	done

	del_namespaces

	return "${KSFT_PASS}"
}

test_ns_diff_global_host_connect_to_global_vm_ok() {
	local pids pid pidfile
	local ns0 ns1 port
	declare -a pids
	local unixfile
	ns0="global0"
	ns1="global1"
	port=1234
	local rc

	init_namespaces

	pidfile=$(mktemp $PIDFILE_TEMPLATE)

	if ! vm_start "${pidfile}" "${ns0}"; then
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
	host_vsock_test "${ns1}" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
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

test_ns_diff_global_host_connect_to_local_vm_fails() {
	local ns0="global0"
	local ns1="local0"
	local port=12345
	local pidfile
	local result
	local pid

	init_namespaces

	outfile=$(mktemp)

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns1}"; then
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

test_ns_diff_global_vm_connect_to_global_host_ok() {
	local ns0="global0"
	local ns1="global1"
	local port=12345
	local unixfile
	local pidfile
	local pids

	init_namespaces

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

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns0}"; then
		log_host "failed to start vm (cid=${cid}, ns=${ns0})"
		terminate_pids "${pids[@]}"
		rm -f "${unixfile}"
		return $KSFT_FAIL
	fi

	vm_wait_for_ssh "${ns0}"
	vm_vsock_test "${ns0}" "10.0.2.2" 2 "${port}"
	rc=$?

	terminate_pidfiles "${pidfile}"
	terminate_pids "${pids[@]}"
	rm -f "${unixfile}"

	if [[ ! $rc -eq 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"

}

test_ns_diff_global_vm_connect_to_local_host_fails() {
	local ns0="global0"
	local ns1="local0"
	local port=12345
	local pidfile
	local result
	local pid

	init_namespaces

	log_host "Launching socat in ns ${ns1}"
	outfile=$(mktemp)
	ip netns exec "${ns1}" socat VSOCK-LISTEN:${port} STDOUT &> "${outfile}" &
	pid=$!

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns0}"; then
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

test_ns_diff_local_host_connect_to_local_vm_fails() {
	local ns0="local0"
	local ns1="local1"
	local port=12345
	local pidfile
	local result
	local pid

	init_namespaces

	outfile=$(mktemp)

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns1}"; then
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

test_ns_diff_local_vm_connect_to_local_host_fails() {
	local ns0="local0"
	local ns1="local1"
	local port=12345
	local pidfile
	local result
	local pid

	init_namespaces

	log_host "Launching socat in ns ${ns1}"
	outfile=$(mktemp)
	ip netns exec "${ns1}" socat VSOCK-LISTEN:"${port}" STDOUT &> "${outfile}" &
	pid=$!

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns0}"; then
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

unload_module() {
	local module=$1
	local i

	for ((i = 0; i < 5; i++)); do
		modprobe -r "${module}" 2>/dev/null || :

		if [[ "$(lsmod | grep -c ${module})" -eq 0 ]]; then
			return 0
		fi

		sleep 1
	done

	return 1
}

__test_loopback_two_netns() {
	local ns0=$1
	local ns1=$2
	local port=12345
	local result
	local pid

	modprobe vsock_loopback &> /dev/null || :

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

test_ns_diff_global_to_local_loopback_local_fails() {
	init_namespaces

	if ! __test_loopback_two_netns "global0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_diff_local_to_global_loopback_fails() {
	init_namespaces

	if ! __test_loopback_two_netns "local0" "global0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_diff_local_to_local_loopback_fails() {
	init_namespaces

	if ! __test_loopback_two_netns "local0" "local1"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_diff_global_to_global_loopback_ok() {
	init_namespaces

	if __test_loopback_two_netns "global0" "global1"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_same_local_loopback_ok() {
	init_namespaces

	if __test_loopback_two_netns "local0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_same_local_host_connect_to_local_vm_ok() {
	local ns="local0"
	local port=1234
	local pidfile
	local rc

	init_namespaces

	pidfile=$(mktemp $PIDFILE_TEMPLATE)

	if ! vm_start "${pidfile}" "${ns}"; then
		return "${KSFT_FAIL}"
	fi

	vm_vsock_test "${ns}" "server" 2 "${TEST_GUEST_PORT}"
	host_vsock_test "${ns}" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
	rc=$?

	terminate_pidfiles "${pidfile}"

	if [[ $rc -ne 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_ns_same_local_vm_connect_to_local_host_ok() {
	local ns="local0"
	local port=1234
	local pidfile
	local rc

	init_namespaces

	pidfile=$(mktemp $PIDFILE_TEMPLATE)

	if ! vm_start "${pidfile}" "${ns}"; then
		return "${KSFT_FAIL}"
	fi

	vm_vsock_test "${ns}" "server" 2 "${TEST_GUEST_PORT}"
	host_vsock_test "${ns}" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"
	rc=$?

	terminate_pidfiles "${pidfile}"

	if [[ $rc -ne 0 ]]; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

namespaces_can_boot_same_cid() {
	local ns0=$1
	local ns1=$2
	local pidfile1 pidfile2
	local rc

	pidfile1=$(mktemp $PIDFILE_TEMPLATE)
	vm_start "${pidfile1}" "${ns0}"

	pidfile2=$(mktemp $PIDFILE_TEMPLATE)
	vm_start "${pidfile2}" "${ns1}"

	rc=$?
	terminate_pidfiles "${pidfile1}" "${pidfile2}"

	return $rc
}

test_ns_global_same_cid_fails() {
	init_namespaces

	if namespaces_can_boot_same_cid "global0" "global1"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_ns_local_global_same_cid_ok() {
	init_namespaces

	if namespaces_can_boot_same_cid "local0" "global0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_global_local_same_cid_ok() {
	init_namespaces

	if namespaces_can_boot_same_cid "global0" "local0"; then
		return "${KSFT_PASS}"
	fi

	return "${KSFT_FAIL}"
}

test_ns_local_same_cid_ok() {
	init_namespaces

	if namespaces_can_boot_same_cid "local0" "local0"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_ns_host_vsock_ns_mode_write_once_ok() {
	add_namespaces

	for mode in "${NS_MODES[@]}"; do
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

test_vm_server_host_client() {
	if ! vm_vsock_test "init_ns" "server" 2 "${TEST_GUEST_PORT}"; then
		return "${KSFT_FAIL}"
	fi

	if ! host_vsock_test "init_ns" "127.0.0.1" "${VSOCK_CID}" "${TEST_HOST_PORT}"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_vm_client_host_server() {
	if ! host_vsock_test "init_ns" "server" "${VSOCK_CID}" "${TEST_HOST_PORT_LISTENER}"; then
		return "${KSFT_FAIL}"
	fi

	if ! vm_vsock_test "init_ns" "10.0.2.2" 2 "${TEST_HOST_PORT_LISTENER}"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

test_vm_loopback() {
	local port=60000 # non-forwarded local port

	vm_ssh "init_ns" -- modprobe vsock_loopback &> /dev/null || :

	if ! vm_vsock_test "init_ns" "server" 1 "${port}"; then
		return "${KSFT_FAIL}"
	fi


	if ! vm_vsock_test "init_ns" "127.0.0.1" 1 "${port}"; then
		return "${KSFT_FAIL}"
	fi

	return "${KSFT_PASS}"
}

check_ns_changes_dont_break_connection() {
	local ns0="global0"
	local ns1="global1"
	local port=12345
	local pidfile
	local outfile
	local pids=()
	local rc=0

	init_namespaces

	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	if ! vm_start "${pidfile}" "${ns0}"; then
		return "${KSFT_FAIL}"
	fi
	vm_wait_for_ssh "${ns0}"

	outfile=$(mktemp)
	vm_ssh "${ns0}" -- \
		socat VSOCK-LISTEN:"${port}",fork STDOUT > "${outfile}" 2>/dev/null &
	pids+=($!)

	# wait_for_listener() does not work for vsock because vsock does not
	# export socket state to /proc/net/. Instead, we have no choice but to
	# sleep for some hardcoded time.
	sleep ${WAIT_PERIOD}

	# We use a pipe here so that we can echo into the pipe instead of
	# using socat and a unix socket file.
	local pipefile=$(mktemp -u /tmp/vmtest_pipe_XXXX)
	ip netns exec "${ns1}" \
		socat PIPE:"${pipefile}" VSOCK-CONNECT:"${VSOCK_CID}":"${port}" &
	pids+=($!)

	timeout ${WAIT_PERIOD} \
		bash -c 'while [[ ! -e '"${pipefile}"' ]]; do sleep 1; done; exit 0'

	if [[ $2 == "delete" ]]; then
		if [[ "$1" == "vm" ]]; then
			ip netns del "${ns0}"
		elif [[ "$1" == "host" ]]; then
			ip netns del "${ns1}"
		elif [[ "$1" == "both" ]]; then
			ip netns del "${ns0}"
			ip netns del "${ns1}"
		fi
	elif [[ $2 == "change_mode" ]]; then
		if [[ "$1" == "vm" ]]; then
			ns_set_mode "${ns0}" "local"
		elif [[ "$1" == "host" ]]; then
			ns_set_mode "${ns1}" "local"
		elif [[ "$1" == "both" ]]; then
			ns_set_mode "${ns0}" "local"
			ns_set_mode "${ns1}" "local"
		fi
	fi

	echo "TEST" > "${pipefile}"

	timeout ${WAIT_PERIOD} \
		bash -c 'while [[ ! -s '"${outfile}"' ]]; do sleep 1; done; exit 0'

	if grep -q "TEST" "${outfile}"; then
		rc="${KSFT_PASS}"
	else
		rc="${KSFT_FAIL}"
	fi

	terminate_pidfiles "${pidfile}"
	terminate_pids "${pids[@]}"
	rm -f "${outfile}"

	return "${rc}"
}

test_ns_mode_change_connection_continue_vm_ok() {
	check_ns_changes_dont_break_connection "vm" "change_mode"
}

test_ns_mode_change_connection_continue_host_ok() {
	check_ns_changes_dont_break_connection "host" "change_mode"
}

test_ns_mode_change_connection_continue_both_ok() {
	check_ns_changes_dont_break_connection "both" "change_mode"
}

test_ns_delete_vm_ok() {
	check_ns_changes_dont_break_connection "vm" "delete"
}

test_ns_delete_host_ok() {
	check_ns_changes_dont_break_connection "host" "delete"
}

test_ns_delete_both_ok() {
	check_ns_changes_dont_break_connection "both" "delete"
}

test_ns_loopback_global_global_late_module_load_ok() {
	declare -a pids
	local unixfile
	local ns0 ns1
	local pids
	local port

	if ! unload_module vsock_loopback; then
		log_host "Unable to unload vsock_loopback, skipping..."
		return "${KSFT_SKIP}"
	fi

	ns0=loopback_ns0
	ns1=loopback_ns1

	ip netns del "${ns0}" &>/dev/null || :
	ip netns del "${ns1}" &>/dev/null || :
	ip netns add "${ns0}"
	ip netns add "${ns1}"
	ns_set_mode "${ns0}" global
	ns_set_mode "${ns1}" global
	ip netns exec "${ns0}" ip link set dev lo up
	ip netns exec "${ns1}" ip link set dev lo up

	modprobe vsock_loopback &> /dev/null || :

	unixfile=$(mktemp -u /tmp/XXXX.sock)
	port=321
	ip netns exec "${ns1}" \
		socat TCP-LISTEN:"${port}",fork \
			UNIX-CONNECT:"${unixfile}" &
	pids+=($!)

	host_wait_for_listener "${ns1}" "${port}"
	ip netns exec "${ns0}" socat UNIX-LISTEN:"${unixfile}",fork \
		TCP-CONNECT:localhost:"${port}" &
	pids+=($!)

	if ! host_vsock_test "${ns0}" "server" 1 "${port}"; then
		ip netns del "${ns0}" &>/dev/null || :
		ip netns del "${ns1}" &>/dev/null || :
		terminate_pids "${pids[@]}"
		return "${KSFT_FAIL}"
	fi

	if ! host_vsock_test "${ns1}" "127.0.0.1" 1 "${port}"; then
		ip netns del "${ns0}" &>/dev/null || :
		ip netns del "${ns1}" &>/dev/null || :
		terminate_pids "${pids[@]}"
		return "${KSFT_FAIL}"
	fi

	ip netns del "${ns0}" &>/dev/null || :
	ip netns del "${ns1}" &>/dev/null || :
	terminate_pids "${pids[@]}"

	return "${KSFT_PASS}"
}

test_ns_loopback_local_local_late_module_load_fails() {
	declare -a pids
	local ns0 ns1
	local outfile
	local pids
	local rc

	if ! unload_module vsock_loopback; then
		log_host "Unable to unload vsock_loopback, skipping..."
		return "${KSFT_SKIP}"
	fi

	ns0=loopback_ns0
	ns1=loopback_ns1

	ip netns del "${ns0}" &>/dev/null || :
	ip netns del "${ns1}" &>/dev/null || :
	ip netns add "${ns0}"
	ip netns add "${ns1}"
	ns_set_mode "${ns0}" local
	ns_set_mode "${ns1}" local

	modprobe vsock_loopback &> /dev/null || :

	outfile=$(mktemp /tmp/XXXX.vmtest.out)
	ip netns exec "${ns0}" socat VSOCK-LISTEN:${port} STDOUT \
		> "${outfile}" 2>/dev/null &
	pids+=($!)

	echo TEST | \
		ip netns exec "${ns1}" socat STDIN VSOCK-CONNECT:1:${port} \
			2>/dev/null

	if grep -q "TEST" "${outfile}" 2>/dev/null; then
		rc="${KSFT_FAIL}"
	else
		rc="${KSFT_PASS}"
	fi

	ip netns del "${ns0}" &>/dev/null || :
	ip netns del "${ns1}" &>/dev/null || :
	terminate_pids "${pids[@]}"
	rm -f "${outfile}"

	return "${rc}"
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

shared_vm_tests_requested() {
	for arg in "$@"; do
		if shared_vm_test "${arg}"; then
			return 0
		fi
	done

	return 1
}

run_shared_vm_tests() {
	local arg

	for arg in "$@"; do
		if ! shared_vm_test "${arg}"; then
			continue
		fi

		if ! check_netns "${arg}"; then
			check_result "${KSFT_SKIP}"
			continue
		fi

		run_shared_vm_test "${arg}"
		check_result $?
	done
}

run_shared_vm_test() {
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
	host_warn_cnt_before=$(dmesg --level=warn | grep -c -i 'vsock')
	vm_oops_cnt_before=$(vm_ssh "init_ns" -- dmesg | grep -c -i 'Oops')
	vm_warn_cnt_before=$(vm_ssh "init_ns" -- dmesg --level=warn | grep -c -i 'vsock')

	name=$(echo "${1}" | awk '{ print $1 }')
	eval test_"${name}"
	rc=$?

	host_oops_cnt_after=$(dmesg | grep -i 'Oops' | wc -l)
	if [[ ${host_oops_cnt_after} -gt ${host_oops_cnt_before} ]]; then
		echo "FAIL: kernel oops detected on host" | log_host
		rc=$KSFT_FAIL
	fi

	host_warn_cnt_after=$(dmesg --level=warn | grep -c -i vsock)
	if [[ ${host_warn_cnt_after} -gt ${host_warn_cnt_before} ]]; then
		echo "FAIL: kernel warning detected on host" | log_host
		rc=$KSFT_FAIL
	fi

	vm_oops_cnt_after=$(vm_ssh "init_ns" -- dmesg | grep -i 'Oops' | wc -l)
	if [[ ${vm_oops_cnt_after} -gt ${vm_oops_cnt_before} ]]; then
		echo "FAIL: kernel oops detected on vm" | log_host
		rc=$KSFT_FAIL
	fi

	vm_warn_cnt_after=$(vm_ssh "init_ns" -- dmesg --level=warn | grep -c -i vsock)
	if [[ ${vm_warn_cnt_after} -gt ${vm_warn_cnt_before} ]]; then
		echo "FAIL: kernel warning detected on vm" | log_host
		rc=$KSFT_FAIL
	fi

	return "${rc}"
}

run_tests() {
	for arg in "${ARGS[@]}"; do
		if shared_vm_test "${arg}"; then
			continue
		fi

		if ! check_netns "${arg}"; then
			check_result "${KSFT_SKIP}"
			continue
		fi

		add_namespaces

		name=$(echo "${arg}" | awk '{ print $1 }')
		log_host "Executing test_${name}"
		eval test_"${name}"
		check_result $?

		del_namespaces
	done
}

BUILD=0
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
check_socat
handle_build

echo "1..${#ARGS[@]}"

cnt_pass=0
cnt_fail=0
cnt_skip=0
cnt_total=0

if shared_vm_tests_requested "${ARGS[@]}"; then
	log_host "Booting up VM"
	pidfile=$(mktemp $PIDFILE_TEMPLATE)
	vm_start "${pidfile}" "init_ns"
	vm_wait_for_ssh "init_ns"
	log_host "VM booted up"

	run_shared_vm_tests "${ARGS[@]}"
	terminate_pidfiles "${pidfile}"
fi

run_tests "${ARGS[@]}"

echo "SUMMARY: PASS=${cnt_pass} SKIP=${cnt_skip} FAIL=${cnt_fail}"
echo "Log: ${LOG}"

if [ $((cnt_pass + cnt_skip)) -eq ${cnt_total} ]; then
	exit "$KSFT_PASS"
else
	exit "$KSFT_FAIL"
fi
