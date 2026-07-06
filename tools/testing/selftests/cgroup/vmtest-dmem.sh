#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2026 Red Hat, Inc.
#
# Run cgroup test_dmem inside a virtme-ng VM.
# Dependencies:
#		* virtme-ng
#		* qemu	(used by virtme-ng)

set -euo pipefail

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT="$(realpath "${SCRIPT_DIR}"/../../../../)"

source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

BUILD=0
QEMU="qemu-system-$(uname -m)"
VERBOSE=0
SHELL_MODE=0
GUEST_TREE="${GUEST_TREE:-$KERNEL_CHECKOUT}"

VM_SCRIPT=""

function usage() {
	cat <<EOF
$0 [OPTIONS]
Options:
	-b	Build kernel from source tree before booting
	-q	QEMU binary/path (default: ${QEMU})
	-s	Start interactive shell in VM instead of running tests
	-v	Verbose output (vng boot logs on stdout)
	-h	Display this help

If you build your kernel using KBUILD_OUTPUT= or O= options, these
can be passed as environment variables to the script:

  O=<build_path> $0 -b

or

  KBUILD_OUTPUT=<build_path> $0 -b

O= takes precedence over KBUILD_OUTPUT= if both are set.
EOF
}

function cleanup() {
	rm -f "${VM_SCRIPT}"
}
trap cleanup EXIT

function skip() {
	local msg=${1:-""}

	ktap_test_skip "${msg}"
	exit "${KSFT_SKIP}"
}

function fail() {
	local msg=${1:-""}

	ktap_test_fail "${msg}"
	exit "${KSFT_FAIL}"
}

function check_deps() {
	for dep in vng "${QEMU}"; do
		if ! command -v "${dep}" >/dev/null 2>&1; then
			skip "dependency ${dep} not found"
		fi
	done
}

function handle_build() {
	[[ "${BUILD}" -eq 1 ]] || return 0

	[[ -f "${KERNEL_CHECKOUT}/kernel/cgroup/dmem_selftest.c" ]] || \
		fail "-b requires vmtest-dmem.sh called from the kernel source tree"

	# Figure out where the kernel is being built.
	# O takes precedence over KBUILD_OUTPUT.
	local out_args=()
	if [[ -n "${O:-}" ]]; then
		out_args=(O="${O}")
	elif [[ -n "${KBUILD_OUTPUT:-}" ]]; then
		out_args=(KBUILD_OUTPUT="${KBUILD_OUTPUT}")
	fi

	pushd "${KERNEL_CHECKOUT}" &>/dev/null
	vng --kconfig --config "${SCRIPT_DIR}"/config "${out_args[@]}" || \
		fail "failed to generate .config for kernel source tree (${KERNEL_CHECKOUT})"
	make "${out_args[@]}" -j"$(nproc 2>/dev/null || echo 1)" || \
		fail "failed to build kernel from source tree (${KERNEL_CHECKOUT})"
	popd &>/dev/null
}

# Run vng with common flags. Extra arguments are appended by the caller:
#   --exec <script>  for automated test runs
#   (nothing)        for interactive shell mode
function run_vm() {
	local vng_args=()

	[[ "${BUILD}" -eq 1 ]] && vng_args+=("${KERNEL_CHECKOUT}")
	[[ "${VERBOSE}" -eq 1 ]] && vng_args+=("--verbose")

	vng \
		--run \
		"${vng_args[@]}" \
		--qemu="$(command -v "${QEMU}")" \
		--user root \
		--rw \
		"$@"
}

function main() {
	while getopts ':hvq:sb' opt; do
		case "${opt}" in
		v) VERBOSE=1 ;;
		q) QEMU="${OPTARG}" ;;
		b) BUILD=1 ;;
		s) SHELL_MODE=1 ;;
		h) usage; exit 0 ;;
		*) usage; exit 1 ;;
		esac
	done

	check_deps
	handle_build

	if [[ "${SHELL_MODE}" -eq 1 ]]; then
		echo "Starting interactive shell in VM. Exit to stop VM."
		run_vm
		exit 0
	fi

	# Write the VM-side script into the script directory so it is
	# accessible in the guest via the --rw host filesystem mount.
	VM_SCRIPT="$(mktemp --suffix=.sh "${SCRIPT_DIR}/.dmem_vmtest_XXXX")"

	cat > "${VM_SCRIPT}" << EOF
#!/bin/bash
set -euo pipefail

mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

# Verify cgroup controllers are available.
if ! grep -q dmem /sys/fs/cgroup/cgroup.controllers || \
   ! grep -q memory /sys/fs/cgroup/cgroup.controllers; then
	echo "guest kernel missing CONFIG_CGROUP_DMEM or CONFIG_MEMCG" >&2
	exit 1
fi

# Load dmem_selftest: try built-in, then modprobe, then build + insmod.
if [[ -e /sys/kernel/debug/dmem_selftest/charge ]]; then
	echo "dmem_selftest ready (built-in or already loaded)"
elif modprobe -q dmem_selftest 2>/dev/null && \
     [[ -e /sys/kernel/debug/dmem_selftest/charge ]]; then
	echo "dmem_selftest ready (modprobe)"
else
	kdir="/lib/modules/\$(uname -r)/build"
	if [[ -d "\$kdir" ]]; then
		echo "Building dmem_selftest.ko against running guest kernel..."
		if make -C "\$kdir" M="${GUEST_TREE}/kernel/cgroup" \
				CONFIG_DMEM_SELFTEST=m modules; then
			insmod "${GUEST_TREE}/kernel/cgroup/dmem_selftest.ko" \
				2>/dev/null || modprobe -q dmem_selftest 2>/dev/null || true
		fi
	fi
	if [[ ! -e /sys/kernel/debug/dmem_selftest/charge ]]; then
		echo "dmem_selftest unavailable (modprobe/build+insmod failed)" >&2
		exit 1
	fi
	echo "dmem_selftest ready (built + insmod)"
fi

echo "Running cgroup/test_dmem in VM..."
cd "${GUEST_TREE}"
make -C tools/testing/selftests TARGETS=cgroup
./tools/testing/selftests/cgroup/test_dmem
EOF

	echo "Booting virtme-ng VM..."
	run_vm --exec "bash ${VM_SCRIPT}"
}

main "$@"
