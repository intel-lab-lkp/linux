#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2026 Red Hat
#
# Dependencies:
#		* virtme-ng
#		* qemu	(used by virtme-ng)

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT=$(realpath "${SCRIPT_DIR}"/../../../../)
readonly CGROUP_DIR="${KERNEL_CHECKOUT}/tools/testing/selftests/cgroup"

source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

readonly DMABUF_HEAP_TEST="${SCRIPT_DIR}"/dmabuf-heap
readonly MEMCONTROL_TEST="${CGROUP_DIR}"/test_memcontrol
readonly TMP_DIR=$(mktemp -d /tmp/dmabuf-vmtest.XXXXXXXX)

VERBOSE=false
BUILD=false
BUILD_HOST=""
BUILD_HOST_PODMAN_CONTAINER_NAME=""
SHELL_MODE=false

usage() {
	echo
	echo "$0 [OPTIONS]"
	echo
	echo "Options"
	echo "  -b: build the kernel from the current source tree and use it for the VM"
	echo "  -H: hostname for remote build host (used with -b)"
	echo "  -p: podman container name for remote build host (used with -b)"
	echo "      Example: -H beefyserver -p vng"
	echo "  -s: drop into an interactive shell in the VM instead of running tests"
	echo "  -v: enable verbose vng/qemu output"
	echo

	exit 1
}

die() {
	echo "$*" >&2
	exit "${KSFT_FAIL}"
}

cleanup() {
	rm -rf "${TMP_DIR}"
}

check_deps() {
	for dep in vng make; do
		if [[ ! -x $(command -v "${dep}") ]]; then
			echo -e "skip:    dependency ${dep} not found!\n"
			exit "${KSFT_SKIP}"
		fi
	done

	if ${SHELL_MODE}; then
		return
	fi

	if [[ ! -x "${DMABUF_HEAP_TEST}" ]]; then
		printf "skip:    %s not found!" "${DMABUF_HEAP_TEST}"
		printf " Please build the kselftest dmabuf-heaps target (or use -b).\n"
		exit "${KSFT_SKIP}"
	fi

	if [[ ! -x "${MEMCONTROL_TEST}" ]]; then
		printf "skip:    %s not found!" "${MEMCONTROL_TEST}"
		printf " Please build the kselftest cgroup target (or use -b).\n"
		exit "${KSFT_SKIP}"
	fi
}

check_vng() {
	local tested_versions=("1.36" "1.37")
	local version
	local ok=0

	version="$(vng --version)"
	for tv in "${tested_versions[@]}"; do
		if [[ "${version}" == *"${tv}"* ]]; then
			ok=1
			break
		fi
	done

	if [[ "${ok}" -eq 0 ]]; then
		printf "warning: vng version '%s' has not been tested and may " "${version}" >&2
		printf "not function properly.\n\tThe following versions have been tested: " >&2
		echo "${tested_versions[@]}" >&2
	fi
}

build_selftests() {
	make -C "${KERNEL_CHECKOUT}" headers_install \
		INSTALL_HDR_PATH="${TMP_DIR}/usr" -j"$(nproc)"

	local khdr="-isystem ${TMP_DIR}/usr/include"

	if ! make -C "${SCRIPT_DIR}" KHDR_INCLUDES="${khdr}" -j"$(nproc)"; then
		die "failed to build dmabuf-heaps selftests"
	fi

	if ! make -C "${CGROUP_DIR}" KHDR_INCLUDES="${khdr}" \
		"${MEMCONTROL_TEST}" -j"$(nproc)"; then
		die "failed to build cgroup/test_memcontrol selftest"
	fi
}

handle_build() {
	if ! ${BUILD}; then
		return
	fi

	if [[ ! -d "${KERNEL_CHECKOUT}" ]]; then
		echo "-b requires vmtest.sh called from the kernel source tree" >&2
		exit 1
	fi

	pushd "${KERNEL_CHECKOUT}" &>/dev/null

	if ! vng --kconfig --config "${SCRIPT_DIR}/config"; then
		die "failed to generate .config for kernel source tree (${KERNEL_CHECKOUT})"
	fi

	local vng_args=("-v" "--config" "${SCRIPT_DIR}/config" "--build")

	if [[ -n "${BUILD_HOST}" ]]; then
		vng_args+=("--build-host" "${BUILD_HOST}")
	fi

	if [[ -n "${BUILD_HOST_PODMAN_CONTAINER_NAME}" ]]; then
		vng_args+=("--build-host-exec-prefix" \
			   "podman exec -ti ${BUILD_HOST_PODMAN_CONTAINER_NAME}")
	fi

	if ! vng "${vng_args[@]}"; then
		die "failed to build kernel from source tree (${KERNEL_CHECKOUT})"
	fi

	build_selftests

	popd &>/dev/null
}

make_runner() {
	# virtme-ng shares the host filesystem, so TMP_DIR is accessible
	# inside the VM at the same absolute path.
	cat > "${TMP_DIR}/run_tests.sh" <<-EOF
	#!/bin/sh
	set -u

	# Enable memory controller for child cgroups
	echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null

	PASS=0; FAIL=0; SKIP=0; N=0

	run() {
		name="\$1"; shift
		N=\$((N+1))
		"\$@"; rc=\$?
		if   [ \$rc -eq 0 ]; then echo "ok \$N \$name";        PASS=\$((PASS+1))
		elif [ \$rc -eq 4 ]; then echo "ok \$N \$name # SKIP"; SKIP=\$((SKIP+1))
		else                      echo "not ok \$N \$name";    FAIL=\$((FAIL+1))
		fi
	}

	run "dmabuf-heap charge_fd ioctl"	${DMABUF_HEAP_TEST}
	run "memcontrol dma-buf memcg"	${MEMCONTROL_TEST} test_memcg_dmabuf
	run "memcontrol dma-buf xfer limit" \
		${MEMCONTROL_TEST} test_memcg_dmabuf_xfer_at_limit
	echo "# PASS=\$PASS SKIP=\$SKIP FAIL=\$FAIL"
	[ \$FAIL -eq 0 ]
	EOF
	chmod +x "${TMP_DIR}/run_tests.sh"
}

run_vm() {
	local verbose_opt=""
	local kernel_opt=""

	${VERBOSE} && verbose_opt="--verbose"

	# If we are running from within the kernel source tree, use the kernel
	# source tree as the kernel to boot, otherwise use the running kernel.
	if [[ "$(realpath "$(pwd)")" == "${KERNEL_CHECKOUT}"* ]]; then
		kernel_opt="${KERNEL_CHECKOUT}"
	fi

	if ${SHELL_MODE}; then
		echo "Starting interactive shell in VM. Exit to stop VM."
		vng --run ${kernel_opt} ${verbose_opt} --user root --memory 512M \
			--append "dma_heap.mem_accounting=1" \
			-- bash --noprofile --norc
		exit "${KSFT_PASS}"
	fi

	vng --run ${kernel_opt} ${verbose_opt} --user root --memory 512M \
		--append "dma_heap.mem_accounting=1" \
		--exec "${TMP_DIR}/run_tests.sh"
}

while getopts :hvsbH:p: o
do
	case $o in
	v) VERBOSE=true;;
	b) BUILD=true;;
	s) SHELL_MODE=true;;
	H) BUILD_HOST=$OPTARG;;
	p) BUILD_HOST_PODMAN_CONTAINER_NAME=$OPTARG;;
	h|*) usage;;
	esac
done
shift $((OPTIND-1))

trap cleanup EXIT

check_deps
check_vng
handle_build
make_runner

echo "Booting VM and running tests..."
run_vm
