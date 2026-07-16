// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024, Amazon.com, Inc. or its affiliates. All Rights Reserved
 *
 * Test for KVM_MEMORY_ATTRIBUTES
 */
#include "kvm_util.h"
#include "apic.h"

uint64_t arch_controlled_read(gva_t addr)
{
	uint64_t val;

	asm volatile("mov %[addr], %%rax \n\r"
		     "mov (%%rax), %[val] \n\r"
		     : [val] "=r" (val)
		     : [addr] "m"(addr)
		     : "memory", "rax");

	return val;
}

void arch_controlled_write(gva_t addr, uint64_t val)
{
	asm volatile("mov %[addr], %%rax \n\r"
		     "mov %[val], %%rbx \n\r"
		     "mov %%rbx, (%%rax) \n\r"
		     :: [addr] "m" (addr), [val] "m" (val)
		     : "memory", "rax", "rbx");
}

void arch_controlled_exec(gva_t addr)
{
	asm volatile("mov %[addr], %%rax \n\r"
		     "call *%%rax \n\t"
		     :: [addr] "m"(addr)
		     : "memory", "rax");
}

void arch_write_return_insn(struct kvm_vm *vm, gpa_t vaddr)
{
	memset(addr_gpa2hva(vm, vaddr), 0xc3, 1);
}

void arch_guest_init(void)
{
	x2apic_enable();
}

/*
 * This test validates that, during a page walk, if the page a PTE is placed in
 * is read-only the accesss and dirty bits will not be written. Note There's a
 * slight variation in behaviour between TDP and non-TDP VMs:
 *  - With TDP enabled, KVM issues a fault exit upon observing the non-writable
 *  page.
 *  - With non-TDP, the access bit is not set, but the walk succeeds.
 *
 *  This is aligned with read-only memslots' behaviour.
 */
static void test_memory_access_pte_ro(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr;
	u64 *pte;
	const u64 accessed_mask = PTE_ACCESSED_MASK(&vm->mmu);

	pte = vm_get_pte(vm, vaddr);
	paddr = addr_hva2gpa(vm, pte) & GENMASK(61, vm->page_shift);

	*pte &= ~accessed_mask;
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NW);
	if (test_page(vcpu, TEST_OP_READ, vaddr) < 0) {
		test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr,
				     /* write PTE's accessed bit */
				     KVM_MEMORY_EXIT_FLAG_WRITE);

		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
		test_page_accessible(vcpu, TEST_OP_READ, vaddr);
		TEST_ASSERT_EQ(*pte & accessed_mask, accessed_mask);

		/* Re-run the test, now vaddr is backed by an EPT. */
		*pte &= ~accessed_mask;
		vm_set_memory_attributes(vm, paddr, vm->page_size,
					 KVM_MEMORY_ATTRIBUTE_NW);
		test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr,
				     /* write PTE's accessed bit */
				     KVM_MEMORY_EXIT_FLAG_WRITE);
		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
		test_page_accessible(vcpu, TEST_OP_READ, vaddr);
	} else {
		TEST_ASSERT_EQ(*pte & accessed_mask, 0);
		vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	}
}

/*
 * This test validates that, during a page walk, if the page a PTE is placed in
 * is maked as non-accesible, KVM issues a fault exit.
 */
static void test_memory_access_pte_nr(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr;
	uint64_t *pte;

	pte = vm_get_pte(vm, vaddr);
	paddr = addr_hva2gpa(vm, pte) & GENMASK(61, vm->page_shift);

	vm_set_memory_attributes(vm, paddr, vm->page_size,
				 KVM_MEMORY_ATTRIBUTE_NO_ACCESS);

	test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr, 0);

	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);

	/* Re-run the test, now vaddr is backed by an SPTE. */
	vm_set_memory_attributes(vm, paddr, vm->page_size,
				 KVM_MEMORY_ATTRIBUTE_NO_ACCESS);
	test_page_restricted(vcpu, TEST_OP_READ, vaddr, paddr, 0);
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);
}

static void test_memory_access_sync_spte(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	struct kvm_vm *vm = vcpu->vm;
	gpa_t paddr = addr_gva2gpa(vm, vaddr);
	uint64_t *pte, old_pte, new_pte;

	pte = vm_get_pte(vm, vaddr);
	gpa_t pte_paddr = addr_hva2gpa(vm, pte);
	gpa_t pte_page_paddr = pte_paddr & GENMASK(61, vm->page_shift);
	int pte_offset = pte_paddr - pte_page_paddr;
	virt_pg_map(vm, PTE_VADDR, pte_page_paddr);
	old_pte = *pte;

	/* Set vmaddr as non-executable */
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NX);

	/*
	 * Make sure SPTEs are populated as previous op might have destroyed
	 * them. We new have a non-executable SPTE.
	 */
	test_page_accessible(vcpu, TEST_OP_READ, vaddr);

	/*
	 * Update PTE, make it non-writable and flush TLBs to make sure we go
	 * through the sync_spte path. This should update the SPTE and make it
	 * read-only.
	 */
	new_pte = (old_pte & ~PT_WRITABLE_MASK) | PT_ACCESSED_MASK;
	test_data->expected_val = new_pte;
	test_page_accessible(vcpu, TEST_OP_WRITE, PTE_VADDR + pte_offset);
	TEST_ASSERT_EQ(*pte, new_pte);
	test_page_accessible(vcpu, TEST_OP_INVPLG, vaddr);

	/* The not executable attrs remain valid */
	arch_write_return_insn(vm, vaddr);
	test_page_restricted(vcpu, TEST_OP_EXEC, vaddr, paddr,
			     KVM_MEMORY_EXIT_FLAG_EXEC);

	/* Cleanup */
	*pte = old_pte;
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	test_page_accessible(vcpu, TEST_OP_EXEC, vaddr);
}

static void arch_test_memory_access_pte(struct kvm_vcpu *vcpu, gva_t vaddr)
{
	test_memory_access_pte_nr(vcpu, vaddr);
	test_memory_access_pte_ro(vcpu, vaddr);
	test_memory_access_sync_spte(vcpu, vaddr);
}

#define IPI_VECTOR	 0xfe

static void guest_ipi_handler_hv(struct ex_regs *regs)
{
	ipis_rcvd++;
	wrmsr(HV_X64_MSR_EOI, 1);
}

/*
 * This test verifies that the Hyper-V hypercall exit handler takes memory
 * attributes into account before accessing input data. It coordinates with the
 * guest through the 'TEST_OP_HYPERV_HYPERCALL_INPUT' operation and instructs
 * the guest to issue two PV IPIs. The first PV IPI fails because the input
 * data is held in read-protected memory. Subsequently, the memory protection
 * is lifted, and the second PV IPI succeeds.
 */
static void test_side_channel_hyperv_hypercall_inputs(struct kvm_vcpu *vcpu,
						      gva_t vaddr,
						      size_t size)
{
	struct kvm_vm *vm = vcpu->vm;
	struct hv_send_ipi_ex *ipi_ex = addr_gva2hva(vm, vaddr);
	gpa_t paddr = addr_gva2gpa(vm, test_data->vaddr);

	if (!kvm_has_cap(KVM_CAP_HYPERV_SEND_IPI) ||
	    !kvm_has_cap(KVM_CAP_HYPERV_HCALL_FAULT_EXIT))
		return;

	vm_enable_cap(vcpu->vm, KVM_CAP_HYPERV_HCALL_FAULT_EXIT, 1);

	ipis_rcvd = 0;
	vm_install_exception_handler(vm, IPI_VECTOR, guest_ipi_handler_hv);
	vcpu_set_msr(vcpu, HV_X64_MSR_GUEST_OS_ID, HYPERV_LINUX_OS_ID);

	ipi_ex->vector = IPI_VECTOR;
	ipi_ex->vp_set.format = HV_GENERIC_SET_ALL;
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NO_ACCESS);
	test_page_restricted(vcpu, TEST_OP_HYPERV_HYPERCALL_INPUT, vaddr, paddr,
			     KVM_MEMORY_EXIT_FLAG_READ);
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	vcpu_run_and_inc_stage(vcpu);
}

/*
 * Verifies that the guest page table walker fails the walk if it encounters a
 * page table entry address read-protected by a memory attribute.
 */
static void test_side_channel_emul_page_walks(struct kvm_vcpu *vcpu,
					      gva_t test_vm_addr,
					      size_t size)
{
	const uint64_t pte_addr_mask = GENMASK(51, 12);
	struct kvm_vm *vm = vcpu->vm;
	struct kvm_translation tr;
	int level = PG_LEVEL_1G;
	gpa_t paddr;
	uint64_t *pte;

	pte = vm_get_pte_level(vm, test_vm_addr, &level);
	TEST_ASSERT_EQ(level, PG_LEVEL_1G);
	paddr = *pte & pte_addr_mask;

	tr = vcpu_translate(vcpu, test_vm_addr);
	TEST_ASSERT_EQ(tr.valid, true);
	TEST_ASSERT_EQ(tr.physical_address, addr_gva2gpa(vm, test_vm_addr));

	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NO_ACCESS);
	tr = vcpu_translate(vcpu, test_vm_addr);
	TEST_ASSERT_EQ(tr.valid, false);

	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
}

static void vm_set_vapic_addr(struct kvm_vcpu *vcpu, uint64_t addr)
{
	struct kvm_vapic_addr va;

	va.vapic_addr = addr;
	vcpu_ioctl(vcpu, KVM_SET_VAPIC_ADDR, &va);
}

/*
 * Perform a dummy regs update to issue a KVM_REQ_EVENT. This forces
 * vapic to be synced before entering the guest.
 */
static void vcpu_force_vapic_update(struct kvm_vcpu *vcpu)
{
	struct kvm_regs regs;

	vcpu_regs_get(vcpu, &regs);
	vcpu_regs_set(vcpu, &regs);
}

/*
 * Setup the vapic address on a GPA that is write-protected. Force an vapic
 * update and validate its contents were not changes. Then, lift the write
 * restriction and validate the page's contents are updated.
 */
static void test_side_channel_vapic_addr(struct kvm_vcpu *vcpu)
{
	struct kvm_vm *vm = vcpu->vm;
	gva_t vaddr = vm_alloc_page(vm);
	gpa_t paddr = addr_gva2gpa(vm, vaddr);

	vm_set_vapic_addr(vcpu, paddr);
	test_data->op = TEST_OP_READ;
	test_data->vaddr = vaddr;
	test_data->confirm_read = 1;
	test_data->expected_val = ~0ULL >> 32;
	memset(addr_gva2hva(vm, vaddr), 0xff, sizeof(uint32_t));
	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NW);
	vcpu_run_and_inc_stage(vcpu);

	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	vcpu_force_vapic_update(vcpu);
	test_data->expected_val = 0ULL;
	vcpu_run_and_inc_stage(vcpu);

	vm_set_vapic_addr(vcpu, 0);
	test_data->confirm_read = 0;
}

static void *vcpu_worker(void *data)
{
	struct test_data *test_data = data;
	struct kvm_vcpu *vcpu = test_data->vcpu;

	vcpu_run_and_inc_stage(vcpu);

	return NULL;
}

/*
 * Set up the pvclock page and validate that KVM periodically updates the
 * 'tsc_timestamp' field. Subsequently, make the pvclock page non-writable and
 * verify that the 'tsc_timestamp' field is no longer updated.
 */
static void test_side_channel_pvclock(struct kvm_vcpu *vcpu)
{
	struct kvm_vm *vm = vcpu->vm;
	gva_t vaddr = vm_alloc_page(vm);
	gpa_t paddr = addr_gva2gpa(vm, vaddr);
	vcpu_set_msr(vcpu, MSR_KVM_SYSTEM_TIME_NEW, paddr | 0x1);

	test_data->op = TEST_OP_MONITOR_ADDRESS;
	test_data->vaddr = vaddr + offsetof(struct pvclock_vcpu_time_info, tsc_timestamp);

	pthread_create(&vcpu_thread, NULL, vcpu_worker, test_data);
	usleep(msecs_to_usecs(1000));
	TEST_ASSERT_EQ(pthread_tryjoin_np(vcpu_thread, NULL), 0);

	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NW);
	pthread_create(&vcpu_thread, NULL, vcpu_worker, test_data);
	usleep(msecs_to_usecs(1000));
	TEST_ASSERT_EQ(pthread_tryjoin_np(vcpu_thread, NULL), EBUSY);

	/* Force the 'monitor_address' guest operation to finish */
	test_data->op = TEST_OP_NOP;
	TEST_ASSERT_EQ(pthread_join(vcpu_thread, NULL), 0);
	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
	vcpu_set_msr(vcpu, MSR_KVM_SYSTEM_TIME_NEW, 0);
}

/*
 * Write to MSR_KVM_WALL_CLOCK_NEW and verify that the struct's 'version' field
 * is updated. Subsequently, make the target guest physical address
 * non-writable, and verify the 'version' field isn't updated anymore.
 */
static void test_side_channel_wallclock(struct kvm_vcpu *vcpu)
{
	struct kvm_vm *vm = vcpu->vm;
	gva_t vaddr = vm_alloc_page(vm);
	gpa_t paddr = addr_gva2gpa(vm, vaddr);
	struct pvclock_wall_clock *wc = addr_gva2hva(vm, vaddr);

	wc->version = 0x0;
	vcpu_set_msr(vcpu, MSR_KVM_WALL_CLOCK_NEW, paddr);
	TEST_ASSERT_EQ(wc->version, 2);

	vm_set_memory_attributes(vm, paddr, vm->page_size, KVM_MEMORY_ATTRIBUTE_NW);
	vcpu_set_msr(vcpu, MSR_KVM_WALL_CLOCK_NEW, paddr);
	TEST_ASSERT_EQ(wc->version, 2);

	vm_set_memory_attributes(vm, paddr, vm->page_size, 0);
}

/*
 * Memory attributes are vulnerable to side-channel attacks. This means that
 * any KVM operation initiated by the guest that requires guest memory access
 * (which is the case for most pv-interfaces) needs to consider memory
 * attributes.
 *
 * The following tests validate that this requirement is upheld for a variety
 * of use-cases. These test cases were selected to exercise specific approaches
 * to accessing guest memory, including:
 *
 *  - kvm_read/write_guest()
 *  - gfn_to_hva_cache
 *  - gfn_to_pfn_cache
 *  - Guest page walker
 */
static void arch_test_side_channels(struct kvm_vcpu *vcpu, gva_t test_vm_addr,
			            size_t size)
{
	test_side_channel_hyperv_hypercall_inputs(vcpu, test_vm_addr, size);
	test_side_channel_emul_page_walks(vcpu, test_vm_addr, size);
	test_side_channel_vapic_addr(vcpu);
	test_side_channel_wallclock(vcpu);
	test_side_channel_pvclock(vcpu);
}
