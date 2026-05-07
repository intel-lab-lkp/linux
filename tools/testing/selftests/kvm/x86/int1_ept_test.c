// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test that KVM correctly handles an EPT/NPT fault during delivery of
 * a #DB exception caused by INT1 (ICEBP, opcode 0xF1).
 *
 * The guest executes INT1, which generates #DB. The IDT entry for #DB
 * uses IST1, pointing to a stack page whose backing has been evicted
 * via MADV_DONTNEED. The CPU takes an EPT/NPT violation when pushing
 * the exception frame. KVM must handle the fault and reinject #DB.
 */
#include <sys/mman.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

#define IST_STACK_GPA	0xd0000000ull
#define IST_STACK_SIZE	PAGE_SIZE
/* IST1 offset in the 64-bit TSS */
#define TSS64_IST1_OFFSET	0x24

static volatile bool db_handler_called;
static volatile u64 db_handler_rip;

static void guest_db_handler(struct ex_regs *regs)
{
	db_handler_called = true;
	db_handler_rip = regs->rip;
}

static void guest_code(void)
{
	unsigned long expected_rip;

	db_handler_called = false;

	/*
	 * Execute INT1 (ICEBP). This generates #DB as a trap, so RIP
	 * in the exception frame points after the 0xF1 byte.
	 */
	asm volatile("lea 1f(%%rip), %0\n\t"
		     ".byte 0xf1\n\t"
		     "1:"
		     : "=r"(expected_rip) :: "memory");

	GUEST_ASSERT(db_handler_called);
	GUEST_ASSERT_EQ(db_handler_rip, expected_rip);
	GUEST_SYNC(0);
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	void *ist_hva;
	unsigned int nr_pages;
	struct idt_entry *idt;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	/* Install #DB handler using the normal exception handler table */
	vm_install_exception_handler(vm, DB_VECTOR, guest_db_handler);

	/*
	 * Allocate a separate stack page for IST1. Map it into the guest
	 * so the IDT entry can reference it.
	 */
	nr_pages = vm_calc_num_guest_pages(VM_MODE_DEFAULT, IST_STACK_SIZE);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    IST_STACK_GPA, 1, nr_pages, 0);
	virt_map(vm, IST_STACK_GPA, IST_STACK_GPA, nr_pages);
	ist_hva = addr_gpa2hva(vm, IST_STACK_GPA);

	/*
	 * Write IST1 in the TSS to point to the top of the IST stack.
	 * TSS64 layout: offset 0x24 = IST1 (8 bytes).
	 */
	{
		void *tss_hva = addr_gva2hva(vm, vm->arch.tss);
		u64 ist1_val = IST_STACK_GPA + IST_STACK_SIZE;

		memcpy(tss_hva + TSS64_IST1_OFFSET, &ist1_val, sizeof(ist1_val));
	}

	/*
	 * Set the #DB IDT entry to use IST1. The handler address stays
	 * the same (the framework's idt_handlers stub for vector 1).
	 */
	idt = (struct idt_entry *)addr_gva2hva(vm, vm->arch.idt);
	idt[DB_VECTOR].ist = 1;

	/*
	 * Evict the IST stack page from the EPT/NPT so that when the CPU
	 * tries to push the #DB exception frame, it takes an EPT violation.
	 */
	madvise(ist_hva, IST_STACK_SIZE, MADV_DONTNEED);

	/* Run the guest — it should execute INT1, fault, get reinjected, succeed */
	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_SYNC:
		pr_info("INT1 with EPT fault on IST stack: PASSED\n");
		break;
	default:
		TEST_FAIL("Unexpected ucall");
	}

	kvm_vm_free(vm);
	return 0;
}
