// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test for KVM_X86_DISABLE_EXITS_APERFMPERF
 *
 * Copyright (C) 2025, Google LLC.
 *
 * Test the ability to disable VM-exits for rdmsr of IA32_APERF and
 * IA32_MPERF. When these VM-exits are disabled, reads of these MSRs
 * return the host's values.
 *
 * Note: Requires read access to /dev/cpu/<lpu>/msr to read host MSRs.
 */

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <asm/msr-index.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"

#define NUM_ITERATIONS 100

static int open_dev_msr(int cpu)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
	return open_path_or_exit(path, O_RDONLY);
}

static uint64_t read_dev_msr(int msr_fd, uint32_t msr)
{
	uint64_t data;
	ssize_t rc;

	rc = pread(msr_fd, &data, sizeof(data), msr);
	TEST_ASSERT(rc == sizeof(data), "Read of MSR 0x%x failed", msr);

	return data;
}

static void guest_code(void)
{
	int i;

	for (i = 0; i < NUM_ITERATIONS; i++)
		GUEST_SYNC2(rdmsr(MSR_IA32_APERF), rdmsr(MSR_IA32_MPERF));

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	uint64_t host_aperf_before, host_mperf_before;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int msr_fd;
	int cpu;
	int i;

	cpu = pin_task_to_one_cpu();

	msr_fd = open_dev_msr(cpu);

	/*
	 * This test requires a non-standard VM initialization, because
	 * KVM_ENABLE_CAP cannot be used on a VM file descriptor after
	 * a VCPU has been created.
	 */
	vm = vm_create(1);

	TEST_REQUIRE(vm_check_cap(vm, KVM_CAP_X86_DISABLE_EXITS) &
		     KVM_X86_DISABLE_EXITS_APERFMPERF);

	vm_enable_cap(vm, KVM_CAP_X86_DISABLE_EXITS,
		      KVM_X86_DISABLE_EXITS_APERFMPERF);

	vcpu = vm_vcpu_add(vm, 0, guest_code);

	host_aperf_before = read_dev_msr(msr_fd, MSR_IA32_APERF);
	host_mperf_before = read_dev_msr(msr_fd, MSR_IA32_MPERF);

	for (i = 0; i < NUM_ITERATIONS; i++) {
		uint64_t host_aperf_after, host_mperf_after;
		uint64_t guest_aperf, guest_mperf;
		struct ucall uc;

		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_DONE:
			break;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
		case UCALL_SYNC:
			guest_aperf = uc.args[0];
			guest_mperf = uc.args[1];

			host_aperf_after = read_dev_msr(msr_fd, MSR_IA32_APERF);
			host_mperf_after = read_dev_msr(msr_fd, MSR_IA32_MPERF);

			TEST_ASSERT(host_aperf_before < guest_aperf,
				    "APERF: host_before (0x%" PRIx64 ") >= guest (0x%" PRIx64 ")",
				    host_aperf_before, guest_aperf);
			TEST_ASSERT(guest_aperf < host_aperf_after,
				    "APERF: guest (0x%" PRIx64 ") >= host_after (0x%" PRIx64 ")",
				    guest_aperf, host_aperf_after);
			TEST_ASSERT(host_mperf_before < guest_mperf,
				    "MPERF: host_before (0x%" PRIx64 ") >= guest (0x%" PRIx64 ")",
				    host_mperf_before, guest_mperf);
			TEST_ASSERT(guest_mperf < host_mperf_after,
				    "MPERF: guest (0x%" PRIx64 ") >= host_after (0x%" PRIx64 ")",
				    guest_mperf, host_mperf_after);

			host_aperf_before = host_aperf_after;
			host_mperf_before = host_mperf_after;

			break;
		}
	}

	kvm_vm_free(vm);
	close(msr_fd);

	return 0;
}
