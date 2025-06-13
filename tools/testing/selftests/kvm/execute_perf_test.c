// SPDX-License-Identifier: GPL-2.0
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "kvm_util.h"
#include "test_util.h"
#include "memstress.h"
#include "guest_modes.h"
#include "ucall_common.h"

/* Global variable used to synchronize all of the vCPU threads. */
static int iteration;

/* Set to true when vCPU threads should exit. */
static bool done;

/* The iteration that was last completed by each vCPU. */
static int vcpu_last_completed_iteration[KVM_MAX_VCPUS];

/* Whether to overlap the regions of memory vCPUs access. */
static bool overlap_memory_access;

struct test_params {
	/* The backing source for the region of memory. */
	enum vm_mem_backing_src_type backing_src;

	/* The amount of memory to allocate for each vCPU. */
	uint64_t vcpu_memory_bytes;

	/* The number of vCPUs to create in the VM. */
	int nr_vcpus;

	/* The number of execute iterations the test will run. */
	int iterations;
};

static void assert_ucall(struct kvm_vcpu *vcpu, uint64_t expected_ucall)
{
	struct ucall uc = {};

	TEST_ASSERT(expected_ucall == get_ucall(vcpu, &uc),
		    "Guest exited unexpectedly (expected ucall %" PRIu64
		    ", got %" PRIu64 ")",
		    expected_ucall, uc.cmd);
}

static bool spin_wait_for_next_iteration(int *current_iteration)
{
	int last_iteration = *current_iteration;

	do {
		if (READ_ONCE(done))
			return false;

		*current_iteration = READ_ONCE(iteration);
	} while (last_iteration == *current_iteration);

	return true;
}

static void vcpu_thread_main(struct memstress_vcpu_args *vcpu_args)
{
	struct kvm_vcpu *vcpu = vcpu_args->vcpu;
	int current_iteration = 0;

	while (spin_wait_for_next_iteration(&current_iteration)) {
		vcpu_run(vcpu);
		assert_ucall(vcpu, UCALL_SYNC);
		vcpu_last_completed_iteration[vcpu->id] = current_iteration;
	}
}

static void spin_wait_for_vcpu(struct kvm_vcpu *vcpu, int target_iteration)
{
	while (READ_ONCE(vcpu_last_completed_iteration[vcpu->id]) !=
	       target_iteration) {
		continue;
	}
}

static void run_iteration(struct kvm_vm *vm, const char *description)
{
	struct timespec ts_elapsed;
	struct timespec ts_start;
	struct kvm_vcpu *vcpu;
	int next_iteration;

	/* Kick off the vCPUs by incrementing iteration. */
	next_iteration = ++iteration;

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	/* Wait for all vCPUs to finish the iteration. */
	list_for_each_entry(vcpu, &vm->vcpus, list)
		spin_wait_for_vcpu(vcpu, next_iteration);

	ts_elapsed = timespec_elapsed(ts_start);
	pr_info("%-30s: %ld.%09lds\n",
		description, ts_elapsed.tv_sec, ts_elapsed.tv_nsec);
}

static void run_test(enum vm_guest_mode mode, void *arg)
{
	struct test_params *params = arg;
	struct kvm_vm *vm;
	int i;

	vm = memstress_create_vm(mode, params->nr_vcpus,
				 params->vcpu_memory_bytes, 1,
				 params->backing_src, !overlap_memory_access);

	memstress_start_vcpu_threads(params->nr_vcpus, vcpu_thread_main);

	pr_info("\n");

	memstress_set_write_percent(vm, 100);
	run_iteration(vm, "Populating memory");

	run_iteration(vm, "Writing to memory");

	memstress_set_execute(vm, true);
	for (i = 0; i < params->iterations; ++i)
		run_iteration(vm, "Executing from memory");

	/* Set done to signal the vCPU threads to exit */
	done = true;

	memstress_join_vcpu_threads(params->nr_vcpus);
	memstress_destroy_vm(vm);
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-h] [-m mode] [-b vcpu_bytes] [-v nr_vcpus] [-o] "
	       "[-s mem_type] [-i iterations]\n",
	       name);
	puts("");
	printf(" -h: Display this help message.");
	guest_modes_help();
	printf(" -b: specify the size of the memory region which should be\n"
	       "     dirtied by each vCPU. e.g. 10M or 3G.\n"
	       "     (default: 1G)\n");
	printf(" -i: specify the number iterations to execute from memory.\n");
	printf(" -v: specify the number of vCPUs to run.\n");
	printf(" -o: Overlap guest memory accesses instead of partitioning\n"
	       "     them into a separate region of memory for each vCPU.\n");
	backing_src_help("-s");
	puts("");
	exit(0);
}

int main(int argc, char *argv[])
{
	struct test_params params = {
		.backing_src = DEFAULT_VM_MEM_SRC,
		.vcpu_memory_bytes = DEFAULT_PER_VCPU_MEM_SIZE,
		.nr_vcpus = 1,
		.iterations = 1,
	};
	int opt;

	guest_modes_append_default();

	while ((opt = getopt(argc, argv, "hm:b:i:v:os:")) != -1) {
		switch (opt) {
		case 'm':
			guest_modes_cmdline(optarg);
			break;
		case 'b':
			params.vcpu_memory_bytes = parse_size(optarg);
			break;
		case 'i':
			params.iterations = atoi(optarg);
			break;
		case 'v':
			params.nr_vcpus = atoi(optarg);
			break;
		case 'o':
			overlap_memory_access = true;
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

	for_each_guest_mode(run_test, &params);

	return 0;
}
