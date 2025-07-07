// SPDX-License-Identifier: GPL-2.0-only
/*
 * nx_huge_pages_perf_test
 *
 * Copyright (C) 2025, Google LLC.
 *
 * Performance test for NX hugepage recovery.
 *
 * This test checks for long faults on allocated pages when NX huge page
 * recovery is taking place on pages mapped by the VM.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_util.h"

#include "kvm_util.h"
#include "processor.h"
#include "ucall_common.h"

/* Default guest test virtual memory offset */
#define DEFAULT_GUEST_TEST_MEM		0xc0000000

/* Default size (2GB) of the memory for testing */
#define DEFAULT_TEST_MEM_SIZE		(2 << 30)

/*
 * Guest virtual memory offset of the testing memory slot.
 * Must not conflict with identity mapped test code.
 */
static uint64_t guest_test_virt_mem = DEFAULT_GUEST_TEST_MEM;

static struct kvm_vcpu *vcpu;

struct test_params {
	enum vm_mem_backing_src_type backing_src;
	uint64_t memory_bytes;
};

struct guest_args {
	uint64_t guest_page_size;
	uint64_t pages;
};

static struct guest_args guest_args;

#define RETURN_OPCODE 0xC3

static void guest_code(int vcpu_idx)
{
	struct guest_args *args = &guest_args;
	uint64_t page_size = args->guest_page_size;
	uint64_t max_cycles = 0UL;
	volatile char *gva;
	uint64_t page;


	for (page = 0; page < args->pages; ++page) {
		gva = (volatile char *)guest_test_virt_mem + page * page_size;

		/*
		 * To time the jitter on all faults on pages that are not
		 * undergoing nx huge page recovery, only execute on every
		 * other 1G region, and only time the non-executing pass.
		 */
		if (page & (1UL << 18)) {
			uint64_t tsc1, tsc2;

			tsc1 = rdtsc();
			*gva = 0;
			tsc2 = rdtsc();

			if (tsc2 - tsc1 > max_cycles)
				max_cycles = tsc2 - tsc1;
		} else {
			*gva = RETURN_OPCODE;
			((void (*)(void)) gva)();
		}
	}

	GUEST_SYNC1(max_cycles);
}

struct kvm_vm *create_vm(uint64_t memory_bytes,
			 enum vm_mem_backing_src_type backing_src)
{
	uint64_t backing_src_pagesz = get_backing_src_pagesz(backing_src);
	struct guest_args *args = &guest_args;
	uint64_t guest_num_pages;
	uint64_t region_end_gfn;
	uint64_t gpa, size;
	struct kvm_vm *vm;

	args->guest_page_size = getpagesize();

	guest_num_pages = vm_adjust_num_guest_pages(VM_MODE_DEFAULT,
				memory_bytes / args->guest_page_size);

	TEST_ASSERT(memory_bytes % getpagesize() == 0,
		    "Guest memory size is not host page size aligned.");

	vm = __vm_create_with_one_vcpu(&vcpu, guest_num_pages, guest_code);

	/* Put the test region at the top guest physical memory. */
	region_end_gfn = vm->max_gfn + 1;

	/*
	 * If there should be more memory in the guest test region than there
	 * can be pages in the guest, it will definitely cause problems.
	 */
	TEST_ASSERT(guest_num_pages < region_end_gfn,
		    "Requested more guest memory than address space allows.\n"
		    "    guest pages: %" PRIx64 " max gfn: %" PRIx64
		    " wss: %" PRIx64 "]",
		    guest_num_pages, region_end_gfn - 1, memory_bytes);

	gpa = (region_end_gfn - guest_num_pages - 1) * args->guest_page_size;
	gpa = align_down(gpa, backing_src_pagesz);

	size = guest_num_pages * args->guest_page_size;
	pr_info("guest physical test memory: [0x%lx, 0x%lx)\n",
		gpa, gpa + size);

	/*
	 * Pass in MAP_POPULATE, because we are trying to test how long
	 * we have to wait for a pending NX huge page recovery to take.
	 * We do not want to also wait for GUP itself.
	 */
	vm_mem_add(vm, backing_src, gpa, 1,
		   guest_num_pages, 0, -1, 0, MAP_POPULATE);

	virt_map(vm, guest_test_virt_mem, gpa, guest_num_pages);

	args->pages = guest_num_pages;

	/* Export the shared variables to the guest. */
	sync_global_to_guest(vm, guest_args);

	return vm;
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct timespec ts_elapsed;
	struct timespec ts_start;
	struct ucall uc = {};
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	ret = _vcpu_run(vcpu);

	ts_elapsed = timespec_elapsed(ts_start);

	TEST_ASSERT(ret == 0, "vcpu_run failed: %d", ret);

	TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_SYNC,
		    "Invalid guest sync status: %" PRIu64, uc.cmd);

	pr_info("Duration: %ld.%09lds\n",
		ts_elapsed.tv_sec, ts_elapsed.tv_nsec);
	pr_info("Max fault latency: %" PRIu64 " cycles\n", uc.args[0]);
}

static void run_test(struct test_params *params)
{
	/*
	 * The fault + execute pattern in the guest relies on having more than
	 * 1GiB to use.
	 */
	TEST_ASSERT(params->memory_bytes > PAGE_SIZE << 18,
		    "Must use more than 1GiB of memory.");

	create_vm(params->memory_bytes, params->backing_src);

	pr_info("\n");

	run_vcpu(vcpu);
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-h] [-b bytes] [-s mem_type]\n",
	       name);
	puts("");
	printf(" -h: Display this help message.");
	printf(" -b: specify the size of the memory region which should be\n"
	       "     dirtied by the guest. e.g. 2048M or 3G.\n"
	       "     (default: 2G, must be greater than 1G)\n");
	backing_src_help("-s");
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	struct test_params params = {
		.backing_src = DEFAULT_VM_MEM_SRC,
		.memory_bytes = DEFAULT_TEST_MEM_SIZE,
	};
	int opt;

	while ((opt = getopt(argc, argv, "hb:s:")) != -1) {
		switch (opt) {
		case 'b':
			params.memory_bytes = parse_size(optarg);
			break;
		case 's':
			params.backing_src = parse_backing_src_type(optarg);
			break;
		case 'h':
		default:
			help(argv[0]);
			break;
		}
	}

	run_test(&params);
}
