// SPDX-License-Identifier: GPL-2.0
/*
 * The kvmclock drift test. Emulate vCPU hotplug and online to verify if
 * there is kvmclock drift.
 *
 * Adapted from steal_time.c
 *
 * Copyright (C) 2020, Red Hat, Inc.
 * Copyright (C) 2024 Oracle and/or its affiliates.
 */

#include <asm/kvm_para.h>
#include <asm/pvclock.h>
#include <asm/pvclock-abi.h>
#include <sys/stat.h>

#include "kvm_util.h"
#include "processor.h"

#define NR_VCPUS		2
#define NR_SLOTS		2
#define KVMCLOCK_SIZE		sizeof(struct pvclock_vcpu_time_info)
/*
 * KVMCLOCK_GPA is identity mapped
 */
#define KVMCLOCK_GPA		(1 << 30)

static uint64_t kvmclock_gpa = KVMCLOCK_GPA;

static void guest_code(int cpu)
{
	struct pvclock_vcpu_time_info *kvmclock;

	/*
	 * vCPU#0 is to detect the change of pvclock_vcpu_time_info
	 */
	if (cpu == 0) {
		GUEST_SYNC(0);

		kvmclock = (struct pvclock_vcpu_time_info *) kvmclock_gpa;
		wrmsr(MSR_KVM_SYSTEM_TIME_NEW, kvmclock_gpa | KVM_MSR_ENABLED);

		/*
		 * Backup the pvclock_vcpu_time_info before vCPU#1 hotplug
		 */
		kvmclock[1] = kvmclock[0];

		GUEST_SYNC(2);
		/*
		 * Enter the guest to update pvclock_vcpu_time_info
		 */
		GUEST_SYNC(4);
	}

	/*
	 * vCPU#1 is to emulate the vCPU hotplug
	 */
	if (cpu == 1) {
		GUEST_SYNC(1);
		/*
		 * This is after the host side MSR_IA32_TSC
		 */
		GUEST_SYNC(3);
	}
}

static void run_vcpu(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
	case UCALL_DONE:
		break;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_ASSERT(false, "Unexpected exit: %s",
			    exit_reason_str(vcpu->run->exit_reason));
	}
}

static void kvmclock_dump(struct pvclock_vcpu_time_info *kvmclock)
{
	pr_info("  version:           %u\n", kvmclock->version);
	pr_info("  tsc_timestamp:     %lu\n", kvmclock->tsc_timestamp);
	pr_info("  system_time:       %lu\n", kvmclock->system_time);
	pr_info("  tsc_to_system_mul: %u\n", kvmclock->tsc_to_system_mul);
	pr_info("  tsc_shift:         %d\n", kvmclock->tsc_shift);
	pr_info("  flags:             %u\n", kvmclock->flags);
	pr_info("\n");
}

#define CLOCKSOURCE_PATH "/sys/devices/system/clocksource/clocksource0/current_clocksource"

static void check_clocksource(void)
{
	char *clk_name;
	struct stat st;
	FILE *fp;

	fp = fopen(CLOCKSOURCE_PATH, "r");
	if (!fp) {
		pr_info("failed to open clocksource file: %d; assuming TSC.\n",
			errno);
		return;
	}

	if (fstat(fileno(fp), &st)) {
		pr_info("failed to stat clocksource file: %d; assuming TSC.\n",
			errno);
		goto out;
	}

	clk_name = malloc(st.st_size);
	TEST_ASSERT(clk_name, "failed to allocate buffer to read file\n");

	if (!fgets(clk_name, st.st_size, fp)) {
		pr_info("failed to read clocksource file: %d; assuming TSC.\n",
			ferror(fp));
		goto out;
	}

	TEST_ASSERT(!strncmp(clk_name, "tsc\n", st.st_size),
		    "clocksource not supported: %s", clk_name);
out:
	fclose(fp);
}

int main(int argc, char *argv[])
{
	struct pvclock_vcpu_time_info *kvmclock;
	struct kvm_vcpu *vcpus[NR_VCPUS];
	uint64_t clock_old, clock_new;
	bool verbose = false;
	unsigned int gpages;
	struct kvm_vm *vm;
	int period = 2;
	uint64_t tsc;
	int opt;

	check_clocksource();

	while ((opt = getopt(argc, argv, "p:vh")) != -1) {
		switch (opt) {
		case 'p':
			period = atoi_positive("The period (seconds) between vCPU hotplug",
					       optarg);
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
		default:
			pr_info("usage: %s [-p period (seconds)] [-v]\n", argv[0]);
			exit(1);
		}
	}

	vm = vm_create_with_vcpus(NR_VCPUS, guest_code, vcpus);
	gpages = vm_calc_num_guest_pages(VM_MODE_DEFAULT,
					 KVMCLOCK_SIZE * NR_SLOTS);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    KVMCLOCK_GPA, 1, gpages, 0);
	virt_map(vm, KVMCLOCK_GPA, KVMCLOCK_GPA, gpages);

	vcpu_args_set(vcpus[0], 1, 0);
	vcpu_args_set(vcpus[1], 1, 1);

	/*
	 * Run vCPU#0 and vCPU#1 to update both pvclock_vcpu_time_info and
	 * master clock
	 */
	run_vcpu(vcpus[0]);
	run_vcpu(vcpus[1]);

	/*
	 * Run vCPU#0 to backup the current pvclock_vcpu_time_info
	 */
	run_vcpu(vcpus[0]);

	sleep(period);

	/*
	 * Emulate the hotplug of vCPU#1
	 */
	vcpu_set_msr(vcpus[1], MSR_IA32_TSC, 0);

	/*
	 * Emulate the online of vCPU#1
	 */
	run_vcpu(vcpus[1]);

	/*
	 * Run vCPU#0 to backup the new pvclock_vcpu_time_info to detect
	 * if there is any change or kvmclock drift
	 */
	run_vcpu(vcpus[0]);

	kvmclock = addr_gva2hva(vm, kvmclock_gpa);
	tsc = kvmclock[0].tsc_timestamp;
	clock_old = __pvclock_read_cycles(&kvmclock[1], tsc);
	clock_new = __pvclock_read_cycles(&kvmclock[0], tsc);

	if (verbose) {
		pr_info("kvmclock based on old pvclock_vcpu_time_info: %lu\n",
			clock_old);
		kvmclock_dump(&kvmclock[1]);
		pr_info("kvmclock based on new pvclock_vcpu_time_info: %lu\n",
			clock_new);
		kvmclock_dump(&kvmclock[0]);
	}

	TEST_ASSERT(clock_old == clock_new,
		    "kvmclock drift detected, old=%lu, new=%lu",
		    clock_old, clock_new);

	kvm_vm_free(vm);

	return 0;
}
