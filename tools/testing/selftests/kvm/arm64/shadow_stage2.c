// SPDX-License-Identifier: GPL-2.0-only
/*
 * shadow_stage2 - Test correctness of shadow stage 2
 */

#include "nested.h"
#include "processor.h"
#include "test_util.h"
#include "ucall.h"

#define XLATE2GPA	(0xABCD)

#define L2SUCCESS	(0x0)
#define L2FAILED	(0x1)
#define L2SYNC		(0x2)

#define TGRAN2NOSUP	(0x3)

/* Used for L2 stack and guest S2 page tables. */
#define L2_PAGE_POOL_ADDR	(0x80000000)
#define L2_PAGE_POOL_NPAGES	(512)
#define L2_PAGE_POOL_MEMSLOT	(0x2)

/*
 * TPIDR_EL2 is used to store vcpu id, so save and restore it.
 */
static gpa_t ucall_translate_to_gpa(void *gva)
{
	gpa_t gpa;
	u64 vcpu_id = read_sysreg(tpidr_el2);

	GUEST_SYNC2(XLATE2GPA, gva);

	/* get the result from userspace */
	gpa = read_sysreg(tpidr_el2);

	write_sysreg(vcpu_id, tpidr_el2);

	return gpa;
}

static void l2_guest_code(void)
{
	do_hvc(L2SYNC, 10, 0);
	do_hvc(L2SYNC, 20, 0);
	do_hvc(L2SYNC, 30, 0);

	do_hvc(L2SUCCESS, 0, 0);
}

static void guest_code(void)
{
	struct vcpu vcpu;
	struct s2_mmu mmu;
	struct hyp_data hyp_data;
	int ret, i = 0;
	gpa_t l2_pc, l2_stack_start, l2_stack_top, s2_pgd;
	gpa_t do_hvc_gpa;
	struct page_pool pp;
	u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);

	GUEST_ASSERT_EQ(get_current_el(), 2);
	GUEST_PRINTF("vEL2 entry\n");

	pp.start = L2_PAGE_POOL_ADDR;
	pp.npages = L2_PAGE_POOL_NPAGES;
	pp.current = L2_PAGE_POOL_ADDR;
	pp.page_size = get_page_size();

	if (!has_tgran_2(mmfr0, pp.page_size))
		GUEST_SYNC1(TGRAN2NOSUP);

	l2_stack_start = alloc_page(&pp);
	l2_stack_top = l2_stack_start + pp.page_size;
	l2_pc = ucall_translate_to_gpa(l2_guest_code);
	do_hvc_gpa = ucall_translate_to_gpa(do_hvc);

	s2_pgd = alloc_page(&pp);

	init_vcpu(&vcpu, l2_pc, l2_stack_top);
	init_s2_mmu(&mmu, 0, s2_pgd, pp.page_size, 40);
	create_s2_mapping(&mmu, l2_pc, l2_pc, pp.page_size * 2, &pp);
	create_s2_mapping(&mmu, do_hvc_gpa, do_hvc_gpa, pp.page_size, &pp);
	create_s2_mapping(&mmu, l2_stack_start, l2_stack_start, pp.page_size, &pp);

	prepare_hyp(&mmu);

	while (true) {
		GUEST_PRINTF("L2 enter\n");
		ret = run_l2(&vcpu, &hyp_data);
		GUEST_PRINTF("L2 exit\n");
		GUEST_ASSERT_EQ(ret, ARM_EXCEPTION_TRAP);
		GUEST_ASSERT_EQ(ESR_ELx_EC(read_sysreg(esr_el2)), ESR_ELx_EC_HVC64);

		if (vcpu.context.regs.regs[0] == L2SYNC)
			GUEST_SYNC3(L2SYNC, i++, vcpu.context.regs.regs[1]);
		else
			break;
	}

	if (vcpu.context.regs.regs[0] != L2SUCCESS)
		GUEST_FAIL("L2 failed\n");

	GUEST_PRINTF("L2 success!\n");
	GUEST_DONE();
}

int main(void)
{
	struct kvm_vcpu_init init;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	gpa_t gpa;

	TEST_REQUIRE(kvm_check_cap(KVM_CAP_ARM_EL2));
	vm = vm_create(1);

	kvm_get_default_vcpu_target(vm, &init);
	init.features[0] |= BIT(KVM_ARM_VCPU_HAS_EL2);
	vcpu = aarch64_vcpu_add(vm, 0, &init, guest_code);
	kvm_arch_vm_finalize_vcpus(vm);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    L2_PAGE_POOL_ADDR, L2_PAGE_POOL_MEMSLOT,
				    L2_PAGE_POOL_NPAGES, 0);
	/*
	 * This idmap allows L1 to traverse and build its guest stage-2, where
	 * it must do a PA to VA conversion in order to descend to the next
	 * level.
	 */
	virt_map(vm, L2_PAGE_POOL_ADDR, L2_PAGE_POOL_ADDR, L2_PAGE_POOL_NPAGES);

	while (true) {
		vcpu_run(vcpu);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			if (uc.args[0] == XLATE2GPA) {
				gpa = addr_gva2gpa(vm, (gva_t)uc.args[1]);
				vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_TPIDR_EL2), gpa);
			}
			if (uc.args[0] == L2SYNC)
				pr_info("L2SYNC, L1 info: %ld, L2 info: %ld\n", uc.args[1], uc.args[2]);
			if (uc.args[0] == TGRAN2NOSUP)
				ksft_exit_skip("Guest page size not supported as guest stage-2 page size!\n");
			break;
		case UCALL_PRINTF:
			pr_info("[L1] %s", uc.buffer);
			break;
		case UCALL_DONE:
			pr_info("DONE!\n");
			goto end;
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			fallthrough;
		default:
			TEST_FAIL("Unhandled ucall: %ld\n", uc.cmd);
		}
	}

end:
	kvm_vm_free(vm);
	return 0;
}
