// SPDX-License-Identifier: GPL-2.0-only
/*
 * vmx_apic_update_test
 *
 * Copyright (C) 2025, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Test L2 guest APIC access page writes with concurrent MMU
 * notification and memslot move updates.
 */
#include <pthread.h>
#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

#define VAPIC_GPA	0xc0000000
#define VAPIC_SLOT	1

#define L2_GUEST_STACK_SIZE 64

#define L2_DELAY	(100)

static void l2_guest_code(void)
{
	uint32_t *vapic_addr = (uint32_t *) (VAPIC_GPA + 0x80);

	/* Unroll the loop to avoid any compiler side effect */

	WRITE_ONCE(*vapic_addr, 1 << 0);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 1);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 2);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 3);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 4);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 5);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 1 << 6);
	udelay(msecs_to_usecs(L2_DELAY));

	WRITE_ONCE(*vapic_addr, 0);
	udelay(msecs_to_usecs(L2_DELAY));

	/* Exit to L1 */
	vmcall();
}

static void l1_guest_code(struct vmx_pages *vmx_pages)
{
	unsigned long l2_guest_stack[L2_GUEST_STACK_SIZE];
	uint32_t control, exit_reason;

	GUEST_ASSERT(prepare_for_vmx_operation(vmx_pages));
	GUEST_ASSERT(load_vmcs(vmx_pages));
	prepare_vmcs(vmx_pages, l2_guest_code,
		     &l2_guest_stack[L2_GUEST_STACK_SIZE]);

	/* Enable APIC access */
	control = vmreadz(CPU_BASED_VM_EXEC_CONTROL);
	control |= CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;
	vmwrite(CPU_BASED_VM_EXEC_CONTROL, control);
	control = vmreadz(SECONDARY_VM_EXEC_CONTROL);
	control |= SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESSES;
	vmwrite(SECONDARY_VM_EXEC_CONTROL, control);
	vmwrite(APIC_ACCESS_ADDR, VAPIC_GPA);

	GUEST_SYNC1(0);
	GUEST_ASSERT(!vmlaunch());
again:
	exit_reason = vmreadz(VM_EXIT_REASON);
	if (exit_reason == EXIT_REASON_APIC_ACCESS) {
		uint64_t guest_rip = vmreadz(GUEST_RIP);
		uint64_t instr_len = vmreadz(VM_EXIT_INSTRUCTION_LEN);

		vmwrite(GUEST_RIP, guest_rip + instr_len);
		GUEST_ASSERT(!vmresume());
		goto again;
	}

	GUEST_SYNC1(exit_reason);
	GUEST_ASSERT(exit_reason == EXIT_REASON_VMCALL);
	GUEST_DONE();
}

static const char *progname;
static int update_period_ms = L2_DELAY / 4;

struct update_control {
	pthread_mutex_t mutex;
	pthread_cond_t start_cond;
	struct kvm_vm *vm;
	bool running;
	bool started;
	int updates;
};

static void wait_for_start_signal(struct update_control *ctrl)
{
	pthread_mutex_lock(&ctrl->mutex);
	while (!ctrl->started)
		pthread_cond_wait(&ctrl->start_cond, &ctrl->mutex);

	pthread_mutex_unlock(&ctrl->mutex);
	printf("%s: starting update\n", progname);
}

static bool is_running(struct update_control *ctrl)
{
	return READ_ONCE(ctrl->running);
}

static void set_running(struct update_control *ctrl, bool running)
{
	WRITE_ONCE(ctrl->running, running);
}

static void signal_thread_start(struct update_control *ctrl)
{
	pthread_mutex_lock(&ctrl->mutex);
	if (!ctrl->started) {
		ctrl->started = true;
		pthread_cond_signal(&ctrl->start_cond);
	}
	pthread_mutex_unlock(&ctrl->mutex);
}

static void *update_madvise(void *arg)
{
	struct update_control *ctrl = arg;
	void *hva;

	wait_for_start_signal(ctrl);

	hva = addr_gpa2hva(ctrl->vm, VAPIC_GPA);
	memset(hva, 0x45, ctrl->vm->page_size);

	while (is_running(ctrl)) {
		usleep(update_period_ms * 1000);
		madvise(hva, ctrl->vm->page_size, MADV_DONTNEED);
		ctrl->updates++;
	}

	return NULL;
}

static void *update_move_memslot(void *arg)
{
	struct update_control *ctrl = arg;
	uint64_t gpa = VAPIC_GPA;

	wait_for_start_signal(ctrl);

	while (is_running(ctrl)) {
		usleep(update_period_ms * 1000);
		gpa += 0x10000;
		vm_mem_region_move(ctrl->vm, VAPIC_SLOT, gpa);
		ctrl->updates++;
	}

	return NULL;
}

static void run(void * (*update)(void *), const char *name)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct vmx_pages *vmx;
	struct update_control ctrl;
	struct ucall uc;
	vm_vaddr_t vmx_pages_gva;
	pthread_t update_thread;
	bool done = false;

	vm = vm_create_with_one_vcpu(&vcpu, l1_guest_code);

	/* Allocate VMX pages */
	vmx = vcpu_alloc_vmx(vm, &vmx_pages_gva);

	/* Allocate memory and create VAPIC memslot */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, VAPIC_GPA,
				    VAPIC_SLOT, 1, 0);

	/* Allocate guest page table */
	virt_map(vm, VAPIC_GPA, VAPIC_GPA, 1);

	/* Set up nested EPT */
	prepare_eptp(vmx, vm);
	nested_map_memslot(vmx, vm, 0);
	nested_map_memslot(vmx, vm, VAPIC_SLOT);
	nested_map(vmx, vm, VAPIC_GPA, VAPIC_GPA, vm->page_size);

	vcpu_args_set(vcpu, 1, vmx_pages_gva);

	pthread_mutex_init(&ctrl.mutex, NULL);
	pthread_cond_init(&ctrl.start_cond, NULL);
	ctrl.vm = vm;
	ctrl.running = true;
	ctrl.started = false;
	ctrl.updates = 0;

	pthread_create(&update_thread, NULL, update, &ctrl);

	printf("%s: running %s (tsc_khz %lu)\n", progname, name, guest_tsc_khz);

	while (!done) {
		vcpu_run(vcpu);

		switch (vcpu->run->exit_reason) {
		case KVM_EXIT_IO:
			switch (get_ucall(vcpu, &uc)) {
			case UCALL_SYNC:
				printf("%s: sync(%ld)\n", progname, uc.args[0]);
				if (uc.args[0] == 0)
					signal_thread_start(&ctrl);
				break;
			case UCALL_ABORT:
				REPORT_GUEST_ASSERT(uc);
				/* NOT REACHED */
			case UCALL_DONE:
				done = true;
				break;
			default:
				TEST_ASSERT(false, "Unknown ucall %lu", uc.cmd);
			}
			break;
		case KVM_EXIT_MMIO:
			/* Handle APIC MMIO access after memslot move */
			printf
			    ("%s: APIC MMIO access at 0x%llx (memslot move effect)\n",
			     progname, vcpu->run->mmio.phys_addr);
			break;
		default:
			TEST_FAIL("%s: Unexpected exit reason: %d (flags 0x%x)",
				  progname,
				  vcpu->run->exit_reason, vcpu->run->flags);
		}
	}

	set_running(&ctrl, false);
	if (!ctrl.started)
		signal_thread_start(&ctrl);
	pthread_join(update_thread, NULL);
	printf("%s: completed with %d updates\n", progname, ctrl.updates);

	pthread_mutex_destroy(&ctrl.mutex);
	pthread_cond_destroy(&ctrl.start_cond);
	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	int opt_madvise = 0;
	int opt_memslot_move = 0;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_VMX));
	TEST_REQUIRE(kvm_cpu_has_ept());

	if (argc == 1) {
		opt_madvise = 1;
		opt_memslot_move = 1;
	} else {
		int opt;

		while ((opt = getopt(argc, argv, "amp:")) != -1) {
			switch (opt) {
			case 'a':
				opt_madvise = 1;
				break;
			case 'm':
				opt_memslot_move = 1;
				break;
			case 'p':
				update_period_ms = atoi(optarg);
				break;
			default:
				exit(1);
			}
		}
	}

	TEST_ASSERT(opt_madvise
		    || opt_memslot_move, "No update test configured");

	progname = argv[0];

	if (opt_madvise)
		run(update_madvise, "madvise");

	if (opt_memslot_move)
		run(update_move_memslot, "move memslot");

	return 0;
}
