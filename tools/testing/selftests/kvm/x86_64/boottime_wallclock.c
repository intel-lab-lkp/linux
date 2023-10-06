// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Oracle and/or its affiliates.
 */

#include <asm/kvm_para.h>
#include <asm/pvclock-abi.h>

#include "kvm_util.h"
#include "processor.h"

static int period = 10;

#define GUEST_SYNC_WALLCLOCK(__stage, __val)                        \
		GUEST_SYNC_ARGS(__stage, __val, 0, 0, 0)

static void guest_main(vm_paddr_t wc_pa, struct pvclock_wall_clock *wc)
{
	uint64_t wallclock;

	while (true) {
		wrmsr(MSR_KVM_WALL_CLOCK_NEW, wc_pa);

		wallclock = wc->sec * NSEC_PER_SEC + wc->nsec;

		GUEST_SYNC_WALLCLOCK(0, wallclock);
	}
}

static void handle_sync(struct ucall *uc)
{
	uint64_t wallclock;

	wallclock = uc->args[2];

	pr_info("Boottime wallclock value: %"PRIu64" ns\n", wallclock);
}

static void handle_abort(struct ucall *uc)
{
	REPORT_GUEST_ASSERT(*uc);
}

static void enter_guest(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	while (true) {
		vcpu_run(vcpu);

		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			handle_sync(&uc);
			break;
		case UCALL_ABORT:
			handle_abort(&uc);
			return;
		default:
			TEST_ASSERT(0, "unhandled ucall: %ld\n", uc.cmd);
			return;
		}

		sleep(period);
	}
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	vm_vaddr_t wc_gva;
	vm_paddr_t wc_gpa;
	int opt;

	while ((opt = getopt(argc, argv, "p:h")) != -1) {
		switch (opt) {
		case 'p':
			period = atoi_positive("The period (seconds)", optarg);
			break;
		case 'h':
		default:
			pr_info("usage: %s [-p period (seconds)]\n", argv[0]);
			exit(1);
		}
	}

	pr_info("Capture boottime wallclock every %d seconds.\n", period);
	pr_info("Stop with Ctrl + c.\n\n");

	vm = vm_create_with_one_vcpu(&vcpu, guest_main);

	wc_gva = vm_vaddr_alloc(vm, getpagesize(), 0x10000);
	wc_gpa = addr_gva2gpa(vm, wc_gva);
	vcpu_args_set(vcpu, 2, wc_gpa, wc_gva);

	enter_guest(vcpu);
	kvm_vm_free(vm);
}
