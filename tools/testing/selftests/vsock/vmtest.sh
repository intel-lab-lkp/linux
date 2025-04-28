#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2025 Meta Platforms, Inc. and affiliates
#
# Dependencies:
#		* virtme-ng
#		* busybox-static (used by virtme-ng)
#		* qemu	(used by virtme-ng)

SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
KERNEL_CHECKOUT=$(realpath ${SCRIPT_DIR}/../../../..)
QEMU=$(command -v qemu-system-$(uname -m))
VERBOSE=0
SKIP_BUILD=0
VSOCK_TEST=${KERNEL_CHECKOUT}/tools/testing/vsock/vsock_test

TEST_GUEST_PORT=51000
TEST_HOST_PORT=50000
TEST_HOST_PORT_LISTENER=50001
SSH_GUEST_PORT=22
SSH_HOST_PORT=2222
VSOCK_CID=1234
WAIT_PERIOD=3
WAIT_PERIOD_MAX=20

QEMU_PIDFILE=/tmp/qemu.pid

# virtme-ng offers a netdev for ssh when using "--ssh", but we also need a
# control port forwarded for vsock_test.  Because virtme-ng doesn't support
# adding an additional port to forward to the device created from "--ssh" and
# virtme-init mistakenly sets identical IPs to the ssh device and additional
# devices, we instead opt out of using --ssh, add the device manually, and also
# add the kernel cmdline options that virtme-init uses to setup the interface.
QEMU_OPTS=""
QEMU_OPTS="${QEMU_OPTS} -netdev user,id=n0,hostfwd=tcp::${TEST_HOST_PORT}-:${TEST_GUEST_PORT}"
QEMU_OPTS="${QEMU_OPTS},hostfwd=tcp::${SSH_HOST_PORT}-:${SSH_GUEST_PORT}"
QEMU_OPTS="${QEMU_OPTS} -device virtio-net-pci,netdev=n0"
QEMU_OPTS="${QEMU_OPTS} -device vhost-vsock-pci,guest-cid=${VSOCK_CID}"
QEMU_OPTS="${QEMU_OPTS} --pidfile ${QEMU_PIDFILE}"
KERNEL_CMDLINE="virtme.dhcp net.ifnames=0 biosdevname=0 virtme.ssh virtme_ssh_user=$USER"

LOG=${SCRIPT_DIR}/vmtest.log

#		Name				Description
avail_tests="
	vm_server_host_client	Run vsock_test in server mode on the VM and in client mode on the host.	
	vm_client_host_server	Run vsock_test in client mode on the VM and in server mode on the host.	
	vm_loopback		Run vsock_test using the loopback transport in the VM.	
"

usage() {
	echo
	echo "$0 [OPTIONS] [TEST]..."
	echo "If no TEST argument is given, all tests will be run."
	echo
	echo "Options"
	echo "  -v: verbose output"
	echo "  -s: skip build"
	echo
	echo "Available tests${avail_tests}"
	exit 1
}

die() {
	echo "$*" >&2
	exit 1
}

vm_ssh() {
	ssh -q -o UserKnownHostsFile=/dev/null -p 2222 localhost $*
	return $?
}

cleanup() {
	if [[ -f "${QEMU_PIDFILE}" ]]; then
		pkill -SIGTERM -F ${QEMU_PIDFILE} 2>&1 >/dev/null
	fi
}

build() {
	log_setup "Building kernel and tests"

	pushd ${KERNEL_CHECKOUT} >/dev/null
	vng \
		--kconfig \
		--config ${KERNEL_CHECKOUT}/tools/testing/selftests/vsock/config.vsock
	make -j$(nproc)
	make -C ${KERNEL_CHECKOUT}/tools/testing/vsock
	popd >/dev/null
	echo
}

vm_setup() {
	local VNG_OPTS=""
	if [[ "${VERBOSE}" = 1 ]]; then
		VNG_OPTS="--verbose"
	fi
	vng \
		$VNG_OPTS \
		--run ${KERNEL_CHECKOUT} \
		--qemu-opts="${QEMU_OPTS}" \
		--qemu="${QEMU}" \
		--user root \
		--append "${KERNEL_CMDLINE}" \
		--rw  2>&1 >/dev/null &
}

vm_wait_for_ssh() {
	i=0
	while [[ true ]]; do
		if [[ ${i} > ${WAIT_PERIOD_MAX} ]]; then
			die "Timed out waiting for guest ssh"
		fi
		vm_ssh -- true
		if [[ $? -eq 0 ]]; then
			break
		fi
		i=$(( i + 1 ))
		sleep ${WAIT_PERIOD}
	done
}

wait_for_listener() {
	local PORT=$1
	local i=0
	while ! ss -ltn | grep -q ":${PORT}"; do
		if [[ ${i} > ${WAIT_PERIOD_MAX} ]]; then
			die "Timed out waiting for listener on port ${PORT}"
		fi
		i=$(( i + 1 ))
		sleep ${WAIT_PERIOD}
	done
}

vm_wait_for_listener() {
	local port=$1
	vm_ssh -- "$(declare -f wait_for_listener); wait_for_listener ${port}"
}

host_wait_for_listener() {
	wait_for_listener ${TEST_HOST_PORT_LISTENER}
}

log() {
	local prefix="$1"
	shift

	if [[ "$#" -eq 0 ]]; then
		cat | awk '{ printf "%s:\t%s\n","'"${prefix}"'", $0 }' | tee -a ${LOG}
	else
		echo "$*" | awk '{ printf "%s:\t%s\n","'"${prefix}"'", $0 }' | tee -a ${LOG}
	fi
}

log_setup() {
	log "setup" "$@"
}

log_host() {
	testname=$1
	shift
	log "test:${testname}:host" "$@"
}

log_guest() {
	testname=$1
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

	vm_wait_for_listener ${TEST_GUEST_PORT}

	${VSOCK_TEST} \
		--mode=client \
		--control-host=127.0.0.1 \
		--peer-cid="${VSOCK_CID}" \
		--control-port="${TEST_HOST_PORT}" 2>&1 | log_host "${testname}"

	return $?
}

test_vm_client_host_server() {
	local testname="${FUNCNAME[0]#test_}"

	${VSOCK_TEST} \
		--mode "server" \
		--control-port "${TEST_HOST_PORT_LISTENER}" \
		--peer-cid "${VSOCK_CID}" 2>&1 | log_host "${testname}" &

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

	vm_ssh -- ${VSOCK_TEST} \
		--mode=server \
		--control-port="${port}" \
		--peer-cid=1 2>&1 | log_guest "${testname}" &

	vm_wait_for_listener ${port}

	vm_ssh -- ${VSOCK_TEST} \
		--mode=client \
		--control-host="127.0.0.1" \
		--control-port="${port}" \
		--peer-cid=1 2>&1 | log_guest "${testname}"

	return $?
}

run_test() {
	unset IFS
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

	host_oops_cnt_after=$(dmesg | grep -i 'Oops' | wc -l)
	if [[ ${host_oops_cnt_after} > ${host_oops_cnt_before} ]]; then
		echo "${name}: kernel oops detected on host" | log_host ${name}
		rc=1
	fi

	host_warn_cnt_after=$(dmesg --level=warn | wc -l)
	if [[ ${host_warn_cnt_after} > ${host_warn_cnt_before} ]]; then
		echo "${name}: kernel warning detected on host" | log_host ${name}
		rc=1
	fi

	vm_oops_cnt_after=$(vm_ssh -- dmesg | grep -i 'Oops' | wc -l)
	if [[ ${vm_oops_cnt_after} > ${vm_oops_cnt_before} ]]; then
		echo "${name}: kernel oops detected on vm" | log_host ${name}
		rc=1
	fi

	vm_warn_cnt_after=$(vm_ssh -- dmesg --level=warn | wc -l)
	if [[ ${vm_warn_cnt_after} > ${vm_warn_cnt_before} ]]; then
		echo "${name}: kernel warning detected on vm" | log_host ${name}
		rc=1
	fi

	return ${rc}
}

while getopts :hvsq: o
do
	case $o in
	v) VERBOSE=1;;
	s) SKIP_BUILD=1;;
	q) QEMU=$OPTARG;;
	h|*) usage;;
	esac
done
shift $((OPTIND-1))

trap cleanup EXIT

if [[ ! -x "$(command -v vng)" ]]; then
	die "vng not found."
fi

if [[ ! -x "${QEMU}" ]]; then
	die "${QEMU} not found."
fi

rm -f "${LOG}"
if [[ "${SKIP_BUILD}" != 1 ]]; then
	build
fi
log_setup "Booting up VM"
vm_setup
vm_wait_for_ssh
log_setup "VM booted up"

for arg in "$@"; do
	if ! command -v > /dev/null "test_${arg}"; then
		echo "Test ${arg} not found"
		die "${usage}"
	fi
done

IFS="	
"
cnt=0
name=""
desc=""
for t in ${avail_tests}; do
	[ "${name}" = "" ] && name="${t}" && continue
	# desc is unused, but we need to eat it.
	[ "${desc}" = "" ] && desc="${t}"

	run_this=0
	if [[ "${#}" -eq 0 ]]; then
		run_this=1
	else
		for arg in "$@"; do
			if [[ "${arg}" = "${name}" ]]; then
				run_this=1
			fi
		done
	fi

	if [[ "${run_this}" = 1 ]]; then
		run_test "${name}"
		rc=$?
		if [[ ${rc} != 0 ]]; then
			cnt=$(( cnt + 1 ))
		fi
	fi
	name=""
	desc=""
done

if [[ ${cnt} = 0 ]]; then
	echo OK
else
	echo FAILED: ${cnt}
fi
echo "Log: ${LOG}"
exit ${cnt}
