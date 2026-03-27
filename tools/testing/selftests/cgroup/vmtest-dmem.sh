#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2026 Red Hat, Inc.
#
# Run cgroup test_dmem inside a virtme-ng VM.
# Dependencies:
#		* virtme-ng
#		* busybox-static (used by virtme-ng)
#		* qemu	(used by virtme-ng)

set -euo pipefail

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT="$(realpath "${SCRIPT_DIR}"/../../../../)"

source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

readonly SSH_GUEST_PORT="${SSH_GUEST_PORT:-22}"
readonly WAIT_PERIOD=3
readonly WAIT_PERIOD_MAX=80
readonly WAIT_TOTAL=$((WAIT_PERIOD * WAIT_PERIOD_MAX))
readonly QEMU_PIDFILE="$(mktemp /tmp/qemu_dmem_vmtest_XXXX.pid)"
readonly QEMU_OPTS=" --pidfile ${QEMU_PIDFILE} "

QEMU="qemu-system-$(uname -m)"
VERBOSE=0
SHELL_MODE=0
GUEST_TREE="${GUEST_TREE:-$KERNEL_CHECKOUT}"

usage() {
	echo
	echo "$0 [OPTIONS]"
	echo "  -q <qemu> QEMU binary/path (default: ${QEMU})"
	echo "  -s        Start interactive shell in VM"
	echo "  -v        Verbose output (use -vv for vng boot logs)"
	echo
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
}

cleanup() {
	if [[ -s "${QEMU_PIDFILE}" ]]; then
		pkill -SIGTERM -F "${QEMU_PIDFILE}" >/dev/null 2>&1 || true
	fi
	rm -f "${QEMU_PIDFILE}"
}

vm_ssh() {
	stdbuf -oL ssh -q \
		-F "${HOME}/.cache/virtme-ng/.ssh/virtme-ng-ssh.conf" \
		-l root "virtme-ng%${SSH_GUEST_PORT}" \
		"$@"
}

check_deps() {
	for dep in vng "${QEMU}" busybox pkill ssh; do
		if ! command -v "${dep}" >/dev/null 2>&1; then
			echo "skip: dependency ${dep} not found"
			exit "${KSFT_SKIP}"
		fi
	done
}

vm_start() {
	local logfile=/dev/null
	local verbose_opt=""

	if [[ "${VERBOSE}" -eq 2 ]]; then
		verbose_opt="--verbose"
		logfile=/dev/stdout
	fi

	vng \
		--run \
		${verbose_opt} \
		--qemu-opts="${QEMU_OPTS}" \
		--qemu="$(command -v "${QEMU}")" \
		--user root \
		--ssh "${SSH_GUEST_PORT}" \
		--rw &>"${logfile}" &

	local vng_pid=$!
	local elapsed=0

	while [[ ! -s "${QEMU_PIDFILE}" ]]; do
		kill -0 "${vng_pid}" 2>/dev/null || die "vng exited early; failed to boot VM"
		[[ "${elapsed}" -ge "${WAIT_TOTAL}" ]] && die "timed out waiting for VM boot"
		sleep 1
		elapsed=$((elapsed + 1))
	done
}

vm_wait_for_ssh() {
	local i=0
	while true; do
		vm_ssh -- true && break
		i=$((i + 1))
		[[ "${i}" -gt "${WAIT_PERIOD_MAX}" ]] && die "timed out waiting for guest ssh"
		sleep "${WAIT_PERIOD}"
	done
}

check_guest_requirements() {
	local cfg_ok
	cfg_ok="$(vm_ssh -- "cfg=/boot/config-\$(uname -r); \
		if [[ -r \"\$cfg\" ]]; then grep -Eq '^CONFIG_CGROUP_DMEM=(y|m)$' \"\$cfg\"; \
		elif [[ -r /proc/config.gz ]]; then \
			zgrep -Eq '^CONFIG_CGROUP_DMEM=(y|m)$' /proc/config.gz; \
		else false; fi; echo \$?")"
	[[ "${cfg_ok}" == "0" ]] || die "guest kernel missing CONFIG_CGROUP_DMEM"
}

setup_guest_dmem_helper() {
	local kdir

	vm_ssh -- "mountpoint -q /sys/kernel/debug || \
		   mount -t debugfs none /sys/kernel/debug" || true

	# Already available (built-in or loaded).
	if vm_ssh -- "[[ -e /sys/kernel/debug/dmem_selftest/charge ]]"; then
		echo "dmem_selftest ready"
		return 0
	fi

	# Fast path: try installed module.
	vm_ssh -- "modprobe -q dmem_selftest 2>/dev/null || true"
	if vm_ssh -- "[[ -e /sys/kernel/debug/dmem_selftest/charge ]]"; then
		echo "dmem_selftest ready"
		return 0
	fi

	# Fallback: build only this module against running guest kernel,
	# then insert it.
	kdir="$(vm_ssh -- "echo /lib/modules/\$(uname -r)/build")"
	if vm_ssh -- "[[ -d '${kdir}' ]]"; then
		echo "Building dmem_selftest.ko against running guest kernel..."
		vm_ssh -- "make -C '${kdir}' \
			M='${GUEST_TREE}/kernel/cgroup' \
			CONFIG_DMEM_SELFTEST=m modules"
		vm_ssh -- "insmod '${GUEST_TREE}/kernel/cgroup/dmem_selftest.ko' \
			2>/dev/null || modprobe -q dmem_selftest 2>/dev/null || true"
	fi

	if vm_ssh -- "[[ -e /sys/kernel/debug/dmem_selftest/charge ]]"; then
		echo "dmem_selftest ready"
		return 0
	fi

	die "dmem_selftest unavailable (modprobe/build+insmod failed)"
}

run_test() {
	vm_ssh -- "cd '${GUEST_TREE}' && make -C tools/testing/selftests TARGETS=cgroup"
	vm_ssh -- "cd '${GUEST_TREE}' && ./tools/testing/selftests/cgroup/test_dmem"
}

while getopts ":hvq:s" o; do
	case "${o}" in
	v) VERBOSE=$((VERBOSE + 1)) ;;
	q) QEMU="${OPTARG}" ;;
	s) SHELL_MODE=1 ;;
	h|*) usage ;;
	esac
done

trap cleanup EXIT

check_deps
echo "Booting virtme-ng VM..."
vm_start
vm_wait_for_ssh
echo "VM is reachable via SSH."
check_guest_requirements
setup_guest_dmem_helper

if [[ "${SHELL_MODE}" -eq 1 ]]; then
	echo "Starting interactive shell in VM. Exit to stop VM."
	vm_ssh -t -- "cd '${GUEST_TREE}' && exec bash --noprofile --norc"
	exit "${KSFT_PASS}"
fi

echo "Running cgroup/test_dmem in VM..."
run_test
echo "PASS: test_dmem completed"
exit "${KSFT_PASS}"
