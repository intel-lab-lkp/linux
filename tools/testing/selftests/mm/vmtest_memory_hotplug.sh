#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test memory_hotplug.offline_migrate_max_passes by reproducing the ACPI
# memory hot-unplug hang in kworker context and verifying that the retry
# limit recovers from it.
#
# offline_pages() retries page migration in an unbounded loop.  A page
# that can never be migrated (long-term pin, slab, busy metadata) makes
# the loop spin forever.  Userspace-driven offlines can be interrupted by
# a signal, but an ACPI DIMM eject runs on the kacpi_hotplug_wq kworker
# where signal_pending() never fires, so the kworker hangs until reboot.
#
# Flow:
#   1. Boot the just-built kernel (virtme-ng) with a cold-plugged pc-dimm.
#   2. Online the DIMM's blocks as ZONE_MOVABLE.
#   3. Pin one page of the DIMM (vmsplice long-term pin).
#   4. Eject the DIMM via sysfs; offline runs on kacpi_hotplug_wq.
#   5. Observe the hang (going-offline persists).
#   6. Rescue: write offline_migrate_max_passes at runtime, verify bailout.
#
# TAP subtests:
#   1  ACPI eject kworker hangs unbounded in offline_pages()
#   2  stuck offline rescued by writing offline_migrate_max_passes
#   3  memory block online and usable after the bailout
#
# Dependencies: virtme-ng (vng), qemu-system-<arch>, cc.
# SKIPs (never FAILs) when tooling or kernel support is missing.

readonly SCRIPT_DIR="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly KERNEL_CHECKOUT="$(realpath "${SCRIPT_DIR}"/../../../../)"

# shellcheck source=../kselftest/ktap_helpers.sh
source "${SCRIPT_DIR}"/../kselftest/ktap_helpers.sh

readonly DIMM_SIZE="${DIMM_SIZE:-256M}"
readonly MAX_PASSES="${MAX_PASSES:-50}"
readonly HANG_HOLD="${HANG_HOLD:-10}"
readonly RESCUE_TIMEOUT="${RESCUE_TIMEOUT:-30}"
readonly BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"

readonly LOG="$(mktemp /tmp/vmtest_mhp_XXXXXX.log)"
readonly GUEST_SCRIPT="$(mktemp /tmp/vmtest_mhp_guest_XXXXXX.sh)"
readonly PIN_SRC="$(mktemp /tmp/vmtest_mhp_pin_XXXXXX.c)"
# Must live in the kernel tree so the guest sees it through the 9p rootfs.
readonly PIN_BIN="${SCRIPT_DIR}/.vmtest_pin_park.$$"

VNG_PID=""

cleanup() {
	[ -n "${VNG_PID}" ] && kill "${VNG_PID}" 2>/dev/null
	rm -f "${LOG}" "${GUEST_SCRIPT}" "${PIN_SRC}" "${PIN_BIN}"
}

guest_result() {
	grep -o "^${1}=[A-Za-z0-9_-]*" "${LOG}" | tail -1 | cut -d= -f2
}

dump_log_tail() {
	tail -25 "${LOG}" | while IFS= read -r line; do
		ktap_print_msg "${line}"
	done
}

skip_all_tests() {
	local i

	for i in 1 2 3; do
		ktap_test_skip "$1"
	done
	ktap_finished
}

# -- pre-flight checks -------------------------------------------------------

ktap_print_header
ktap_set_plan 3
trap cleanup EXIT

arch="$(uname -m)"
case "${arch}" in
x86_64|aarch64) ;;
*)	skip_all_tests "pc-dimm ACPI hotplug not available on ${arch}" ;;
esac

for dep in vng "qemu-system-${arch}" cc; do
	if ! command -v "${dep}" >/dev/null 2>&1; then
		skip_all_tests "${dep} not installed"
	fi
done

# -- pin helper (compiled on the host, visible to the guest via 9p) -----------
#
# Allocate one block's worth of pages (ZONE_MOVABLE serves them from the
# DIMM), find one that landed in a target block, vmsplice-pin it into a
# pipe, print PIN_BLK=<N>, and sleep forever.

cat > "${PIN_SRC}" <<'PINEOF'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/uio.h>

static unsigned long memory_block_size(void)
{
	char buf[64];
	ssize_t n;
	int fd;

	fd = open("/sys/devices/system/memory/block_size_bytes", O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strtoul(buf, NULL, 16);
}

static unsigned long vaddr_to_block(int pagemap_fd, char *vaddr,
				    long page_size, unsigned long blk_size)
{
	unsigned long vpn = (unsigned long)vaddr / page_size;
	uint64_t ent;

	if (pread(pagemap_fd, &ent, sizeof(ent), vpn * 8) != sizeof(ent))
		return -1UL;
	if (!(ent & (1ULL << 63)))
		return -1UL;
	return (ent & ((1ULL << 55) - 1)) * page_size / blk_size;
}

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned long blk_size = memory_block_size();
	unsigned long off, blk = 0;
	char *area, *page = NULL;
	int pagemap_fd, pipefd[2], i;

	if (argc < 2 || !blk_size) {
		fprintf(stderr, "usage: pin_park <memory block>...\n");
		return 1;
	}

	area = mmap(NULL, blk_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (area == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(area, 0x42, blk_size);

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0) {
		perror("pagemap");
		return 1;
	}

	for (off = 0; off < blk_size && !page; off += page_size) {
		blk = vaddr_to_block(pagemap_fd, area + off, page_size,
				     blk_size);
		for (i = 1; i < argc; i++) {
			if (blk == strtoul(argv[i], NULL, 10)) {
				page = area + off;
				break;
			}
		}
	}
	close(pagemap_fd);

	if (!page) {
		printf("PIN_NONE\n");
		fflush(stdout);
		return 2;
	}

	if (pipe(pipefd)) {
		perror("pipe");
		return 1;
	}

	struct iovec iov = { .iov_base = page, .iov_len = page_size };

	if (vmsplice(pipefd[1], &iov, 1, 0) != page_size) {
		perror("vmsplice");
		return 1;
	}

	printf("PIN_BLK=%lu\n", blk);
	fflush(stdout);
	pause();
	return 0;
}
PINEOF

if ! cc -O2 -o "${PIN_BIN}" "${PIN_SRC}" 2>>"${LOG}"; then
	skip_all_tests "cannot build the pin helper"
fi

# -- guest script (runs as init inside the VM) --------------------------------

cat > "${GUEST_SCRIPT}" <<PROLOGUE
#!/bin/bash
readonly MAX_PASSES=${MAX_PASSES}
readonly HANG_HOLD=${HANG_HOLD}
readonly RESCUE_TIMEOUT=${RESCUE_TIMEOUT}
readonly PIN_PARK="${PIN_BIN}"
PROLOGUE

cat >> "${GUEST_SCRIPT}" <<'GUESTEOF'
set -u

readonly PARAM=/sys/module/memory_hotplug/parameters/offline_migrate_max_passes
readonly MEM=/sys/devices/system/memory

skip() { echo "VMTEST_SKIP: $*"; echo "VMTEST_DONE"; exit 0; }

block_state() {
	cat "${MEM}/memory${pinned_blk}/state" 2>/dev/null || echo GONE
}

[ -f "${PARAM}" ] || skip "kernel lacks offline_migrate_max_passes"

# 1. Online the DIMM's blocks as ZONE_MOVABLE.
dimm_blocks=""
for s in "${MEM}"/memory*/state; do
	[ "$(cat "${s}" 2>/dev/null)" = offline ] || continue
	if echo online_movable > "${s}" 2>/dev/null; then
		b=${s%/state}
		dimm_blocks="${dimm_blocks} ${b##*memory}"
	fi
done
echo "# DIMM memory blocks:${dimm_blocks:- none}"
[ -n "${dimm_blocks}" ] || skip "no offline memory blocks (pc-dimm missing?)"

# 2. Pin one page inside the DIMM.
"${PIN_PARK}" ${dimm_blocks} > /tmp/pin.out 2>&1 &
pin_pid=$!
for _ in $(seq 1 10); do
	grep -q "PIN_" /tmp/pin.out 2>/dev/null && break
	sleep 1
done
pinned_blk=$(sed -n 's/^PIN_BLK=//p' /tmp/pin.out)
[ -n "${pinned_blk}" ] || skip "pin failed: $(cat /tmp/pin.out 2>/dev/null)"
echo "# pinned one page in block memory${pinned_blk}"

# 3. Eject the DIMM with the retry limit disabled (default = unbounded).
echo 0 > "${PARAM}"
dmesg -c > /dev/null 2>&1 || true

ejdev=""
for dev in /sys/bus/acpi/devices/PNP0C80:*; do
	[ -e "${dev}/eject" ] && { ejdev="${dev}"; break; }
done
[ -n "${ejdev}" ] || skip "no ejectable ACPI memory device (PNP0C80)"
echo "# ejecting ${ejdev}"
echo 1 > "${ejdev}/eject" &

# 4. Observe the hang: the block must stay in "going-offline".
sleep 3
stuck=0
if [ "$(block_state)" = going-offline ]; then
	echo "# block memory${pinned_blk} going-offline, holding ${HANG_HOLD}s"
	sleep "${HANG_HOLD}"
	if [ "$(block_state)" = going-offline ]; then
		stuck=1
		echo 1 > /proc/sys/kernel/sysrq 2>/dev/null || true
		echo l > /proc/sysrq-trigger 2>/dev/null || true
		sleep 1
		dmesg | grep -B2 -A12 "offline_pages" | tail -20 \
			| sed 's/^/#  /'
	fi
fi
echo "VMTEST_STUCK=${stuck}"

# 5. Rescue: arm the retry limit at runtime and wait for bailout.
echo "${MAX_PASSES}" > "${PARAM}"
echo "# armed offline_migrate_max_passes=$(cat "${PARAM}")"

rescued=0
for _ in $(seq 1 "${RESCUE_TIMEOUT}"); do
	if dmesg | grep -q "giving up after"; then
		rescued=1
		break
	fi
	sleep 1
done
dmesg | grep "giving up after" | tail -2 | sed 's/^/# /'
echo "VMTEST_RESCUED=${rescued}"

sleep 2
echo "VMTEST_BLOCK_STATE=$(block_state)"

kill "${pin_pid}" 2>/dev/null
echo "VMTEST_DONE"
GUESTEOF

# -- launch VM ----------------------------------------------------------------

( cd "${KERNEL_CHECKOUT}" && exec vng --force-9p --no-virtme-ng-init \
	--qemu-opt=-m --qemu-opt="2G,slots=4,maxmem=8G" \
	--qemu-opt=-object \
	--qemu-opt="memory-backend-ram,id=mem0,size=${DIMM_SIZE}" \
	--qemu-opt=-device --qemu-opt="pc-dimm,id=dimm0,memdev=mem0" \
	bash "${GUEST_SCRIPT}" ) > "${LOG}" 2>&1 &
VNG_PID=$!

for _ in $(seq 1 "${BOOT_TIMEOUT}"); do
	grep -q "VMTEST_DONE" "${LOG}" 2>/dev/null && break
	kill -0 "${VNG_PID}" 2>/dev/null || break
	sleep 1
done

if ! grep -q "VMTEST_DONE" "${LOG}" 2>/dev/null; then
	dump_log_tail
	skip_all_tests "VM did not finish within ${BOOT_TIMEOUT}s"
fi

kill "${VNG_PID}" 2>/dev/null
wait "${VNG_PID}" 2>/dev/null
VNG_PID=""

# -- report results -----------------------------------------------------------

skip_reason="$(grep "^VMTEST_SKIP: " "${LOG}" | tail -1 | tr -d '\r')"
if [ -n "${skip_reason}" ]; then
	skip_all_tests "${skip_reason#VMTEST_SKIP: }"
fi

grep "^#" "${LOG}" | tr -d '\r' | while IFS= read -r line; do
	ktap_print_msg "${line#\# }"
done

stuck="$(guest_result VMTEST_STUCK)"
rescued="$(guest_result VMTEST_RESCUED)"
blk_state="$(guest_result VMTEST_BLOCK_STATE)"

if [ "${blk_state:-GONE}" = "GONE" ]; then
	skip_all_tests "DIMM was ejected despite the pin (pin did not hold)"
fi

if [ "${stuck:-0}" = 1 ]; then
	ktap_test_pass "ACPI eject kworker hangs unbounded in offline_pages()"
else
	dump_log_tail
	ktap_test_fail "ACPI eject kworker hangs unbounded in offline_pages()"
fi

if [ "${rescued:-0}" = 1 ]; then
	ktap_test_pass "stuck offline rescued by writing offline_migrate_max_passes"
else
	dump_log_tail
	ktap_test_fail "stuck offline rescued by writing offline_migrate_max_passes"
fi

if [ "${blk_state}" = "online" ]; then
	ktap_test_pass "memory block online and usable after the bailout"
else
	ktap_test_fail "memory block online and usable after the bailout (state=${blk_state})"
fi

ktap_finished
