#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2025 Meta Platforms, Inc. and affiliates
#
# Dependencies:
#		* virtme-ng
#		* busybox-static (used by virtme-ng)
#		* qemu	(used by virtme-ng)

TAP_PREFIX="# "
VERBOSE=0
KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4
SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
VSOCK_TEST=${SCRIPT_DIR}/vsock_test

TEST_GUEST_PORT=51000
TEST_HOST_PORT=50000
TEST_HOST_PORT_LISTENER=50001
SSH_GUEST_PORT=22
SSH_HOST_PORT=2222
VSOCK_CID=1234
WAIT_PERIOD=3
WAIT_PERIOD_MAX=20
WAIT_TOTAL=$(( WAIT_PERIOD * WAIT_PERIOD_MAX ))

QEMU_PIDFILE=/tmp/qemu.pid

# virtme-ng offers a netdev for ssh when using "--ssh", but we also need a
# control port forwarded for vsock_test.  Because virtme-ng doesn't support
# adding an additional port to forward to the device created from "--ssh" and
# virtme-init mistakenly sets identical IPs to the ssh device and additional
# devices, we instead opt out of using --ssh, add the device manually, and also
# add the kernel cmdline options that virtme-init uses to setup the interface.
QEMU_TEST_PORT_FWD="hostfwd=tcp::${TEST_HOST_PORT}-:${TEST_GUEST_PORT}"
QEMU_SSH_PORT_FWD="hostfwd=tcp::${SSH_HOST_PORT}-:${SSH_GUEST_PORT}"
QEMU_OPTS="\
	 -netdev user,id=n0,${QEMU_TEST_PORT_FWD},${QEMU_SSH_PORT_FWD} \
	 -device virtio-net-pci,netdev=n0 \
	 -device vhost-vsock-pci,guest-cid=${VSOCK_CID} \
	 --pidfile ${QEMU_PIDFILE} \
"
KERNEL_CMDLINE="virtme.dhcp net.ifnames=0 biosdevname=0 virtme.ssh virtme_ssh_user=$USER"

LOG=${SCRIPT_DIR}/vmtest.log

TEST_NAMES=(vm_server_host_client vm_client_host_server vm_loopback)
TEST_DESCS=(
	"Run vsock_test in server mode on the VM and in client mode on the host."
	"Run vsock_test in client mode on the VM and in server mode on the host."
	"Run vsock_test using the loopback transport in the VM."
)

usage() {
	local name
	local desc
	local i
	echo
	echo "$0 [OPTIONS] [TEST]..."
	echo "If no TEST argument is given, all tests will be run."
	echo
	echo "Options"
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
	exit ${KSFT_FAIL}
}

vm_ssh() {
	ssh -q -o UserKnownHostsFile=/dev/null -p ${SSH_HOST_PORT} localhost $*
	return $?
}

cleanup() {
	if [[ -f "${QEMU_PIDFILE}" ]]; then
		pkill -SIGTERM -F ${QEMU_PIDFILE} 2>&1 > /dev/null
	fi
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
	for dep in vng ${QEMU} busybox timeout pkill ssh; do
		if [[ ! -x "$(command -v ${dep})" ]]; then
			echo -e "skip:    dependency ${dep} not found!\n"
			exit ${KSFT_SKIP}
		fi
	done

	if [[ ! -x $(command -v ${VSOCK_TEST}) ]]; then
		printf "skip:    ${VSOCK_TEST} not found!"
		printf " Please build the kselftest vsock target.\n"
		exit ${KSFT_SKIP}
	fi
}

vm_setup() {
	local VNG_OPTS=""
	local logfile=/dev/null

	if [[ "${VERBOSE}" = 1 ]]; then
		VNG_OPTS="--verbose"
		logfile=/dev/stdout
	fi
	vng \
		$VNG_OPTS \
		--run \
		--qemu-opts="${QEMU_OPTS}" \
		--qemu="${QEMU}" \
		--user root \
		--append "${KERNEL_CMDLINE}" \
		--rw  &> ${logfile} &

	timeout ${WAIT_TOTAL} \
		bash -c 'while [[ ! -e '"${QEMU_PIDFILE}"' ]]; do sleep 1; done; exit 0'
	if [[ ! $? -eq 0 ]]; then
		die "failed to boot VM"
	fi
}

vm_wait_for_ssh() {
	i=0
	while [[ true ]]; do
		if [[ ${i} -gt ${WAIT_PERIOD_MAX} ]]; then
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
	local port=$1
	local interval=$2
	local max_intervals=$3
	local i=0
	while ! ss -ltn | grep -q ":${port}"; do
		if [[ ${i} -gt ${max_intervals} ]]; then
			die "Timed out waiting for listener on port ${port}"
		fi
		i=$(( i + 1 ))
		sleep ${interval}
	done
}

vm_wait_for_listener() {
	local port=$1
	vm_ssh <<EOF
$(declare -f wait_for_listener)
wait_for_listener ${port} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
EOF
}

host_wait_for_listener() {
	wait_for_listener ${TEST_HOST_PORT_LISTENER} ${WAIT_PERIOD} ${WAIT_PERIOD_MAX}
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
		__log_stdin | tee -a ${LOG} > ${redirect}
	else
		__log_args | tee -a ${LOG} > ${redirect}
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

tap_prefix() {
	sed -e "s/^/${TAP_PREFIX}/"
}

tap_output() {
	if [[ ! -z "$TAP_PREFIX" ]]; then
		read str
		echo $str
	fi
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
	rc=$?

	host_oops_cnt_after=$(dmesg | grep -i 'Oops' | wc -l)
	if [[ ${host_oops_cnt_after} -gt ${host_oops_cnt_before} ]]; then
		echo "${name}: kernel oops detected on host" | log_host ${name}
		rc=$KSFT_FAIL
	fi

	host_warn_cnt_after=$(dmesg --level=warn | wc -l)
	if [[ ${host_warn_cnt_after} -gt ${host_warn_cnt_before} ]]; then
		echo "${name}: kernel warning detected on host" | log_host ${name}
		rc=$KSFT_FAIL
	fi

	vm_oops_cnt_after=$(vm_ssh -- dmesg | grep -i 'Oops' | wc -l)
	if [[ ${vm_oops_cnt_after} -gt ${vm_oops_cnt_before} ]]; then
		echo "${name}: kernel oops detected on vm" | log_host ${name}
		rc=$KSFT_FAIL
	fi

	vm_warn_cnt_after=$(vm_ssh -- dmesg --level=warn | wc -l)
	if [[ ${vm_warn_cnt_after} -gt ${vm_warn_cnt_before} ]]; then
		echo "${name}: kernel warning detected on vm" | log_host ${name}
		rc=$KSFT_FAIL
	fi

	return ${rc}
}

while getopts :hvsq: o
do
	case $o in
	v) VERBOSE=1;;
	q) QEMU=$OPTARG;;
	h|*) usage;;
	esac
done
shift $((OPTIND-1))

trap cleanup EXIT

if [[ ${#} -eq 0 ]]; then
	ARGS=(${TEST_NAMES[@]})
else
	ARGS=($@)
fi

check_args "${ARGS[@]}"
check_deps

QEMU=$(command -v ${QEMU:-qemu-system-$(uname -m)})

echo "TAP version 13" | tap_output
echo "1..${#ARGS[@]}" | tap_output

rm -f "${LOG}"
log_setup "Booting up VM"
vm_setup
vm_wait_for_ssh
log_setup "VM booted up"

cnt_pass=0
cnt_fail=0
cnt_skip=0
cnt_total=0
exitcode=0
for arg in "${ARGS[@]}"; do
	run_test "${arg}"
	rc=$?
	if [[ ${rc} == $KSFT_PASS ]]; then
		cnt_pass=$(( cnt_pass + 1 ))
		echo "[PASS]" | tap_prefix
		echo "ok ${cnt_total} ${arg}" | tap_output
	elif [[ ${rc} == $KSFT_SKIP ]]; then
		cnt_skip=$(( cnt_skip + 1 ))
		echo "[SKIP]" | tap_prefix
		echo "ok ${cnt_total} ${arg} # SKIP" | tap_output
		exitcode=$KSFT_SKIP
	elif [[ ${rc} == $KSFT_FAIL ]]; then
		cnt_fail=$(( cnt_fail + 1 ))
		echo "[FAIL]" | tap_prefix
		echo "not ok ${cnt_fail} ${arg} # exit=$rc" | tap_output
		exitcode=$KSFT_FAIL
	fi
	cnt_total=$(( cnt_total + 1 ))
done

echo "SUMMARY: PASS=${cnt_pass} SKIP=${cnt_skip} FAIL=${cnt_fail}" | tap_prefix
echo "1..${cnt_total}" | tap_output

exit ${rc}
