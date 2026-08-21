#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2026 Red Hat, Inc.
#
# Run cgroup memory controller selftests inside a virtme-ng VM
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
readonly QEMU_PIDFILE="$(mktemp /tmp/qemu_memcg_vmtest_XXXX.pid)"
readonly QEMU_OPTS=" --pidfile ${QEMU_PIDFILE} "

BUILD=0
QEMU="qemu-system-$(uname -m)"
VERBOSE=0
SHELL_MODE=0
GUEST_TREE="${GUEST_TREE:-$KERNEL_CHECKOUT}"

usage() {
	cat <<EOF
$0 [OPTIONS]
    -b        Build kernel from source tree before booting
    -q <qemu> QEMU binary/path (default: $QEMU)
    -s        Start interactive shell in VM
    -v        Verbose output (vng boot logs on stdout)
EOF
	exit 1
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
}

cleanup() {
	if [[ -s "$QEMU_PIDFILE" ]]; then
		pkill -SIGTERM -F "$QEMU_PIDFILE" >/dev/null 2>&1 || true
	fi

	if [[ -e "$QEMU_PIDFILE" ]]; then
		rm -f "$QEMU_PIDFILE"
	fi
}

vm_ssh() {
	stdbuf -oL ssh -q \
		-F "$HOME/.cache/virtme-ng/.ssh/virtme-ng-ssh.conf" \
		-l root "virtme-ng%$SSH_GUEST_PORT" \
		"$@"
}

check_deps() {
	for dep in vng "$QEMU" busybox pkill ssh; do
		if ! command -v "$dep" >/dev/null 2>&1; then
			echo "skip: dependency $dep not found"
			exit "$KSFT_SKIP"
		fi
	done
}

handle_build() {
	if [[ ! "$BUILD" -eq 1 ]]; then
		return
	fi

	if [[ ! -d "$KERNEL_CHECKOUT" ]]; then
		echo "-b requires $0 to be called from the kernel source tree" >&2
		exit 1
	fi

	pushd "$KERNEL_CHECKOUT" &>/dev/null

	if ! vng --kconfig --config "$SCRIPT_DIR"/config; then
		die "failed to generate .config for kernel source tree ($KERNEL_CHECKOUT)"
	fi

	if ! make -j"$(nproc)"; then
		die "failed to build kernel from source tree ($KERNEL_CHECKOUT)"
	fi

	popd &>/dev/null
}

vm_start() {
	local logfile=/dev/null
	local verbose_opt=""
	local kernel_opt=""

	if [[ "$VERBOSE" -eq 1 ]]; then
		verbose_opt="--verbose"
		logfile=/dev/stdout
	fi

	if [[ "$BUILD" -eq 1 ]]; then
		kernel_opt="${KERNEL_CHECKOUT}"
	fi

	vng \
		--run \
		${kernel_opt} \
		${verbose_opt} \
		--qemu-opts="$QEMU_OPTS" \
		--qemu="$(command -v "$QEMU")" \
		--user root \
		--ssh "$SSH_GUEST_PORT" \
		--memory 3G \
		--append "cma=64M hugetlb_cma=1G hugetlb_cma_only=1" \
		--rw &>"$logfile" &

	local vng_pid=$!
	local elapsed=0

	while [[ ! -s "$QEMU_PIDFILE" ]]; do
		kill -0 "$vng_pid" 2>/dev/null || \
			die "vng exited early; failed to boot VM"
		[[ "$elapsed" -ge "$WAIT_TOTAL" ]] && \
			die "timed out waiting for VM boot"
		sleep 1
		elapsed=$((elapsed + 1))
	done
}

vm_wait_for_ssh() {
	local i=0
	while true; do
		vm_ssh -- true && break
		i=$((i + 1))
		[[ "${i}" -gt "$WAIT_PERIOD_MAX" ]] && \
			die "timed out waiting for guest ssh"
		sleep "$WAIT_PERIOD"
	done
}

check_guest_requirements() {
	vm_ssh -- "grep -q memory /sys/fs/cgroup/cgroup.controllers" \
		|| die "memory controller not available (CONFIG_MEMCG?)"
	vm_ssh -- "[[ -e /dev/dma_heap/default_cma_region ]]" \
		|| die "CMA heap not available (CONFIG_DMABUF_HEAPS_CMA? cma= cmdline?)"
}

setup_guest_cma_accounting() {
	vm_ssh -- "mount -o remount,memory_cma_accounting /sys/fs/cgroup" \
		|| die "failed to enable memory_cma_accounting"
	vm_ssh -- "echo +memory > /sys/fs/cgroup/cgroup.subtree_control" \
		|| die "failed to enable memory controller"
}

run_test() {
	vm_ssh -- "cd '$GUEST_TREE' && make -C tools/testing/selftests TARGETS=cgroup"
	vm_ssh -- "cd '$GUEST_TREE' && make -C tools/testing/selftests TARGETS=cgroup run_tests"
}

while getopts ":hvq:sb" opt; do
	case "$opt" in
	v) VERBOSE=1 ;;
	q) QEMU="${OPTARG}" ;;
	b) BUILD=1 ;;
	s) SHELL_MODE=1 ;;
	h|*) usage ;;
	esac
done

trap cleanup EXIT

check_deps
handle_build
echo "Booting virtme-ng VM..."
vm_start
vm_wait_for_ssh
echo "VM is reachable via SSH."

if [[ "$SHELL_MODE" -eq 1 ]]; then
	echo "Starting interactive shell in VM. Exit to stop VM."
	vm_ssh -t -- "cd '$GUEST_TREE' && exec bash --noprofile --norc"
	exit "$KSFT_PASS"
fi

check_guest_requirements
setup_guest_cma_accounting

echo "Running cgroup selftests in VM..."
run_test
echo "PASS: cgroup selftests completed"
exit "$KSFT_PASS"
