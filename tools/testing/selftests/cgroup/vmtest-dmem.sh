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

QEMU="qemu-system-$(uname -m)"
VERBOSE=0
SHELL_MODE=0
GUEST_TREE="${GUEST_TREE:-$KERNEL_CHECKOUT}"

VM_SCRIPT=""

function usage() {
	cat <<EOF
$0 [OPTIONS]
Options:
	-q	QEMU binary/path (default: ${QEMU})
	-s	Start interactive shell in VM instead of running tests
	-v	Verbose output (vng boot logs on stdout)
	-h	Display this help
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

# Run vng with common flags. Extra arguments are appended by the caller:
#   --exec <script>  for automated test runs
#   (nothing)        for interactive shell mode
function run_vm() {
	local verbose_opt=""

	[[ "${VERBOSE}" -eq 1 ]] && verbose_opt="--verbose"

	vng \
		--run \
		${verbose_opt:+"${verbose_opt}"} \
		--qemu="$(command -v "${QEMU}")" \
		--user root \
		--rw \
		"$@"
}

function main() {
	while getopts ':hvq:s' opt; do
		case "${opt}" in
		v) VERBOSE=1 ;;
		q) QEMU="${OPTARG}" ;;
		s) SHELL_MODE=1 ;;
		h) usage; exit 0 ;;
		*) usage; exit 1 ;;
		esac
	done

	check_deps

	if [[ "${SHELL_MODE}" -eq 1 ]]; then
		echo "Starting interactive shell in VM. Exit to stop VM."
		run_vm
		exit 0
	fi

	ktap_print_header
	ktap_set_plan 1

	# Write the VM-side script to a tempfile. Because vng mounts the host
	# filesystem read-write via --rw, the guest can read it at the same path.
	VM_SCRIPT="$(mktemp --suffix=.sh /tmp/dmem_vmtest_XXXX)"

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
	if run_vm --exec "bash ${VM_SCRIPT}"; then
		ktap_test_pass "test_dmem"
	else
		fail "test_dmem"
	fi
}

main "$@"
