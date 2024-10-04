// SPDX-License-Identifier: GPL-2.0-only
/*
 * MMU shrinker test
 *
 * Test MMU shrinker invocation on VMs. This test needs kernel built with
 * shrinker debugfs and mounted. Generally that location is
 * /sys/debug/kernel/shrinker.
 *
 * Test will keep adding and removing memslots while guest is accessing memory
 * so that vCPUs will keep taking fault and filling up caches to process the
 * page faults. It will also invoke shrinker after memslot changes which will
 * race with vCPUs to empty caches.
 *
 * Copyright 2010 Google LLC
 *
 */

#include "guest_modes.h"
#include "kvm_util.h"
#include "memstress.h"
#include "test_util.h"
#include "ucall_common.h"

#include <dirent.h>
#include <error.h>
#include <fnmatch.h>
#include <kselftest.h>
#include <linux/limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SHRINKER_DIR "shrinker"
#define KVM_MMU_SHRINKER_PREFIX "x86-mmu-*"
#define SHRINKER_SCAN_FILE "scan"
#define DUMMY_MEMSLOT_INDEX 10
#define DEFAULT_MMU_SHRINKER_ITERATIONS 5
#define DEFAULT_MMU_SHRINKER_VCPUS 2
#define DEFAULT_MMU_SHRINKER_DELAY_MS 100

struct test_params {
	uint64_t iterations;
	uint64_t guest_percpu_mem_size;
	int delay_ms;
	int nr_vcpus;
	char kvm_shrink_scan_file[PATH_MAX];
};

static int filter(const struct dirent *dir)
{
	return !fnmatch(KVM_MMU_SHRINKER_PREFIX, dir->d_name, 0);
}

static int find_kvm_shrink_scan_path(const char *shrinker_path,
				     char *kvm_shrinker_path, size_t size)
{
	struct dirent **dirs = NULL;
	int ret = 0;
	size_t len;
	int n;

	n = scandir(shrinker_path, &dirs, filter, NULL);
	if (n == -1) {
		return -errno;
	} else if (n != 1) {
		pr_info("Expected one x86-mmu shrinker but found %d\n", n);
		ret = -ENOTSUP;
		goto out;
	}

	len = strnlen(shrinker_path, PATH_MAX) +
	      1 + /* For path separator '/' */
	      strnlen(dirs[0]->d_name, PATH_MAX) +
	      1 + /* For path separator '/' */
	      strnlen(SHRINKER_SCAN_FILE, PATH_MAX);

	if (len >= PATH_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}

	strcpy(kvm_shrinker_path, shrinker_path);
	strcat(kvm_shrinker_path, "/");
	strcat(kvm_shrinker_path, dirs[0]->d_name);
	strcat(kvm_shrinker_path, "/");
	strcat(kvm_shrinker_path, SHRINKER_SCAN_FILE);

out:
	while (n > 0)
		free(dirs[n--]);
	free(dirs);
	return ret;
}

static void find_and_validate_kvm_shrink_scan_file(char *kvm_mmu_shrink_scan_file, size_t size)
{
	char shrinker_path[PATH_MAX];
	int ret;

	ret = find_debugfs_subsystem_path(SHRINKER_DIR, shrinker_path, PATH_MAX);
	if (ret == -ENOENT) {
		pr_info("Cannot find debugfs, error (%d - %s). Skipping the test.\n",
			-ret, strerror(-ret));
		exit(KSFT_SKIP);
	} else if (ret) {
		exit(-ret);
	}

	ret = find_kvm_shrink_scan_path(shrinker_path, kvm_mmu_shrink_scan_file, size);
	if (ret == -ENOENT) {
		pr_info("Cannot find kvm shrinker debugfs path, error (%d - %s). Skipping the test.\n",
			-ret, strerror(-ret));
		exit(KSFT_SKIP);
	} else if (ret) {
		exit(-ret);
	}

	if (access(kvm_mmu_shrink_scan_file, W_OK))
		exit(errno);

	pr_info("Got KVM MMU shrink scan file at: %s\n",
		kvm_mmu_shrink_scan_file);
}

static int invoke_kvm_mmu_shrinker_scan(struct kvm_vm *vm,
					const char *kvm_shrink_scan_file,
					uint64_t iterations, int delay_ms)
{
	uint64_t pages = 1;
	uint64_t gpa;
	FILE *shrinker_scan_fp;
	struct timespec ts;
	int i = 1;

	ts.tv_sec = delay_ms / 1000;
	ts.tv_nsec = (delay_ms - (ts.tv_sec * 1000)) * 1000000;

	gpa = memstress_args.gpa - pages * vm->page_size;

	shrinker_scan_fp = fopen(kvm_shrink_scan_file, "w");
	if (!shrinker_scan_fp) {
		pr_info("Not able to open KVM shrink scan file for writing\n");
		return -errno;
	}

	while (iterations--) {
		/* Adding and deleting memslots rebuilds the page table */
		vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, gpa,
					    DUMMY_MEMSLOT_INDEX, pages, 0);
		vm_mem_region_delete(vm, DUMMY_MEMSLOT_INDEX);

		pr_info("Iteration %d: Invoking shrinker.\n", i++);
		fprintf(shrinker_scan_fp, "0 0 1000\n");
		rewind(shrinker_scan_fp);

		nanosleep(&ts, NULL);
	}

	fclose(shrinker_scan_fp);
	return 0;
}

static void vcpu_worker(struct memstress_vcpu_args *vcpu_args)
{
	struct kvm_vcpu *vcpu = vcpu_args->vcpu;
	struct kvm_run *run;
	int ret;

	run = vcpu->run;

	/* Let the guest access its memory until a stop signal is received */
	while (!READ_ONCE(memstress_args.stop_vcpus)) {
		ret = _vcpu_run(vcpu);
		TEST_ASSERT(ret == 0, "vcpu_run failed: %d", ret);

		if (get_ucall(vcpu, NULL) == UCALL_SYNC)
			continue;

		TEST_ASSERT(false,
			    "Invalid guest sync status: exit_reason=%s\n",
			    exit_reason_str(run->exit_reason));
	}
}

static void run_test(struct test_params *p)
{
	struct kvm_vm *vm;
	int nr_vcpus = p->nr_vcpus;

	pr_info("Creating the VM.\n");
	vm = memstress_create_vm(VM_MODE_DEFAULT, p->nr_vcpus,
				 p->guest_percpu_mem_size,
				 /*slots =*/1, DEFAULT_VM_MEM_SRC,
				 /*partition_vcpu_memory_access=*/true);

	memstress_start_vcpu_threads(p->nr_vcpus, vcpu_worker);

	pr_info("Starting the test.\n");
	invoke_kvm_mmu_shrinker_scan(vm, p->kvm_shrink_scan_file, p->iterations,
				     p->delay_ms);

	pr_info("Test completed.\nStopping the VM.\n");
	memstress_join_vcpu_threads(nr_vcpus);
	memstress_destroy_vm(vm);
}

static void help(char *name)
{
	puts("");
	printf("usage: %s [-b memory] [-d delay_usec] [-i iterations] [-h]\n"
	       "       [-v vcpus] \n", name);
	printf(" -b: specify the size of the memory region which should be\n"
	       "     accessed by each vCPU. e.g. 10M or 3G. (Default: 1G)\n");
	printf(" -d: add a delay between each iterations of firing MMU shrinker\n"
	       "     scan in milliseconds. (Default: %dms).\n",
	       DEFAULT_MMU_SHRINKER_DELAY_MS);
	printf(" -i: specify the number of iterations of firing MMU shrinker.\n"
	       "     scan. (Default: %d)\n",
	       DEFAULT_MMU_SHRINKER_ITERATIONS);
	printf(" -v: specify the number of vCPUs to run. (Default: %d)\n",
	       DEFAULT_MMU_SHRINKER_VCPUS);
	printf(" -h: Print the help message.\n");
	puts("");
}

int main(int argc, char *argv[])
{
	int max_vcpus = kvm_check_cap(KVM_CAP_MAX_VCPUS);
	struct test_params p = {
		.iterations = DEFAULT_MMU_SHRINKER_ITERATIONS,
		.guest_percpu_mem_size = DEFAULT_PER_VCPU_MEM_SIZE,
		.nr_vcpus = DEFAULT_MMU_SHRINKER_VCPUS,
		.delay_ms = DEFAULT_MMU_SHRINKER_DELAY_MS,
	};
	int opt;

	while ((opt = getopt(argc, argv, "b:d:i:v:")) != -1) {
		switch (opt) {
		case 'b':
			p.guest_percpu_mem_size = parse_size(optarg);
			break;
		case 'd':
			p.delay_ms = atoi_non_negative("Time gap between two MMU shrinker invocations in milliseconds",
						       optarg);
			break;
		case 'i':
			p.iterations = atoi_positive("Number of iterations", optarg);
			break;
		case 'v':
			p.nr_vcpus = atoi_positive("Number of vCPUs", optarg);
			TEST_ASSERT(p.nr_vcpus <= max_vcpus,
				    "Invalid number of vcpus, must be between 1 and %d",
				    max_vcpus);
			break;
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	find_and_validate_kvm_shrink_scan_file(p.kvm_shrink_scan_file, PATH_MAX);
	run_test(&p);
	return 0;
}
