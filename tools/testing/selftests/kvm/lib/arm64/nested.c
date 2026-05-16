// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 Nested virtualization helpers
 */

#include "nested.h"
#include "processor.h"
#include "test_util.h"
#include <asm/sysreg.h>
#include <linux/sizes.h>

#define _has_tgran_2(__r, __sz)						\
	({								\
		u64 _s1, _s2, _mmfr0 = __r;				\
									\
		_s2 = SYS_FIELD_GET(ID_AA64MMFR0_EL1,			\
				    TGRAN##__sz##_2, _mmfr0);		\
									\
		_s1 = SYS_FIELD_GET(ID_AA64MMFR0_EL1,			\
				    TGRAN##__sz, _mmfr0);		\
									\
		((_s2 != ID_AA64MMFR0_EL1_TGRAN##__sz##_2_NI &&		\
		  _s2 != ID_AA64MMFR0_EL1_TGRAN##__sz##_2_TGRAN##__sz) || \
		 (_s2 == ID_AA64MMFR0_EL1_TGRAN##__sz##_2_TGRAN##__sz && \
		  _s1 != ID_AA64MMFR0_EL1_TGRAN##__sz##_NI));		\
	})

bool has_tgran_2(u64 mmfr0, size_t size)
{
	switch (size) {
	case SZ_4K:
		return _has_tgran_2(mmfr0, 4);
	case SZ_16K:
		return _has_tgran_2(mmfr0, 16);
	case SZ_64K:
		return _has_tgran_2(mmfr0, 64);
	default:
		return false;
	}
}

size_t get_page_size(void)
{
	u64 tcr_el1 = read_sysreg(tcr_el1);
	u64 tg0 = SYS_FIELD_GET(TCR_EL1, TG0, tcr_el1);

	switch (tg0) {
	case TCR_EL1_TG0_4K:
		return SZ_4K;
	case TCR_EL1_TG0_16K:
		return SZ_16K;
	case TCR_EL1_TG0_64K:
		return SZ_64K;
	default:
		GUEST_FAIL("Unexpected tg0 value!\n");
		return 0;
	}
}

gpa_t alloc_page(struct page_pool *pp)
{
	gpa_t page = pp->current;

	pp->current += pp->page_size;

	if ((pp->current - pp->start) / pp->page_size <= pp->npages) {
		return page;
	} else {
		GUEST_FAIL("%s failed!\n", __func__);
		return 0;
	}
}

void prepare_hyp_no_s2(void)
{
	write_sysreg(HCR_EL2_E2H | HCR_EL2_RW, hcr_el2);
	write_sysreg(hyp_vectors, vbar_el2);
	isb();
}

void prepare_hyp(struct s2_mmu *mmu)
{
	write_sysreg(mmu->vtcr, vtcr_el2);
	write_sysreg(mmu->pgd | ((u64)mmu->vmid << 48), vttbr_el2);
	write_sysreg(HCR_EL2_E2H | HCR_EL2_RW | HCR_EL2_VM, hcr_el2);
	write_sysreg(hyp_vectors, vbar_el2);
	isb();
}

void init_vcpu(struct vcpu *vcpu, gpa_t l2_pc, gpa_t l2_stack_top)
{
	memset(vcpu, 0, sizeof(*vcpu));
	vcpu->context.regs.pc = l2_pc;
	vcpu->context.regs.pstate = PSR_MODE_EL1h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;
	vcpu->context.sys_regs[SP_EL1] = l2_stack_top;
}

static int stage2_levels(unsigned int page_size_shift, u64 ipa_bits)
{
	/* taken from ARM64_HW_PGTABLE_LEVELS(ipa) in KVM */
	return (ipa_bits - 4) / (page_size_shift - 3);
}

static u64 get_index(struct s2_mmu *mmu, u64 ipa, int level)
{
	int width = mmu->page_size_shift - 3;
	int shift_amount = mmu->page_size_shift + (3 - level) * width;

	return (ipa >> shift_amount) & GENMASK_ULL(width - 1, 0);
}

static u64 pte_gpa_to_gva(u64 gpa)
{
	/*
	 * This depends on how the memory used for s2pt is mapped in GVA,
	 * currently it is assumed they are idmapped.
	 */
	return gpa;
}

static u64 pte_to_pt_base(u64 pte)
{
	return pte & GENMASK_ULL(47, 12);
}

#define S2_PTE_AF		(1ULL << 10)
#define S2_PTE_SH_INNER		(3ULL << 8)
#define S2_PTE_S2AP_RW		(3ULL << 6)
#define S2_PTE_ATTR_NORMAL_WB	(0xfULL << 2)
#define S2_PTE_TYPE_TABLE	(1ULL << 1)
#define S2_PTE_TYPE_PAGE	(1ULL << 1)
#define S2_PTE_VALID		1ULL

/* No block mappings for now. */
static void create_one_s2_mapping(struct s2_mmu *mmu, u64 ipa, u64 pa,
				  struct page_pool *pp)
{
	int levels = stage2_levels(mmu->page_size_shift, mmu->ipa_bits);
	u64 index, pte, pte_new, table_attr, page_attr;
	gpa_t pte_addr, pt_base = mmu->pgd;

	table_attr = S2_PTE_TYPE_TABLE | S2_PTE_VALID;
	page_attr = S2_PTE_AF | S2_PTE_SH_INNER | S2_PTE_S2AP_RW |
		    S2_PTE_ATTR_NORMAL_WB | S2_PTE_TYPE_PAGE | S2_PTE_VALID;

	for (int level = 4 - levels; level <= 3; level++) {
		index = get_index(mmu, ipa, level);
		pte_addr = pt_base + index * 8;
		pte = *((u64 *)pte_gpa_to_gva(pte_addr));

		if (level == 3) {
			/* Last level, install leaf entry. */
			pte_new = pa & ~GENMASK_ULL(mmu->page_size_shift - 1, 0);
			pte_new |= page_attr;
			*((u64 *)pte_gpa_to_gva(pte_addr)) = pte_new;
		} else if (!(pte & S2_PTE_VALID)) {
			/* Empty next level table, allocate and install. */
			pte_new = alloc_page(pp);
			pte_new |= table_attr;
			*((u64 *)pte_gpa_to_gva(pte_addr)) = pte_new;
			pt_base = pte_to_pt_base(pte_new);
		} else {
			/* Next level table found, descend into it. */
			pt_base = pte_to_pt_base(pte);
		}
	}
}

void create_s2_mapping(struct s2_mmu *mmu, u64 ipa, u64 pa, size_t size,
		       struct page_pool *pp)
{
	u64 ipa_end;
	u64 mask = pp->page_size - 1;

	ipa_end = (ipa + size + mask) & ~mask;
	ipa &= ~mask;
	pa &= ~mask;

	while (ipa < ipa_end) {
		create_one_s2_mapping(mmu, ipa, pa, pp);
		pa += pp->page_size;
		ipa += pp->page_size;
	}
	dsb(ishst);
}

void init_s2_mmu(struct s2_mmu *mmu, unsigned int vmid, gpa_t pgd,
		 size_t page_size, u64 ipa_bits)
{
	u64 ps, tg0, sl0_base, mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	int levels;

	mmu->vmid = vmid;
	mmu->pgd = pgd;
	mmu->ipa_bits = ipa_bits;
	mmu->vtcr = 0;

	switch (page_size) {
	case SZ_4K:
		tg0 = VTCR_EL2_TG0_4K;
		mmu->page_size_shift = 12;
		sl0_base = 2;
		break;
	case SZ_16K:
		tg0 = VTCR_EL2_TG0_16K;
		mmu->page_size_shift = 14;
		sl0_base = 3;
		break;
	case SZ_64K:
	default:
		tg0 = VTCR_EL2_TG0_64K;
		mmu->page_size_shift = 16;
		sl0_base = 3;
		break;
	}

	levels = stage2_levels(mmu->page_size_shift, mmu->ipa_bits);
	mmu->vtcr |= FIELD_PREP(VTCR_EL2_SL0, (sl0_base - (4 - levels)));

	ps = SYS_FIELD_GET(ID_AA64MMFR0_EL1, PARANGE, mmfr0);
	/* cap ps to 48-bit */
	ps = ps > 0b0101 ? 0b0101 : ps;
	mmu->vtcr |= VTCR_EL2_RES1 | SYS_FIELD_PREP(VTCR_EL2, PS, ps)           |
				    SYS_FIELD_PREP(VTCR_EL2, TG0, tg0)         |
				    SYS_FIELD_PREP_ENUM(VTCR_EL2, SH0, INNER)  |
				    SYS_FIELD_PREP_ENUM(VTCR_EL2, ORGN0, WBWA) |
				    SYS_FIELD_PREP_ENUM(VTCR_EL2, IRGN0, WBWA);

	mmu->vtcr |= FIELD_PREP(VTCR_EL2_T0SZ, 64 - ipa_bits);
}

void __sysreg_save_el1_state(struct cpu_context *ctxt)
{
	ctxt->sys_regs[SP_EL1] = read_sysreg(sp_el1);
}

void __sysreg_restore_el1_state(struct cpu_context *ctxt)
{
	write_sysreg(ctxt->sys_regs[SP_EL1], sp_el1);
}

int run_l2(struct vcpu *vcpu, struct hyp_data *hyp_data)
{
	u64 ret;

	__sysreg_restore_el1_state(&vcpu->context);

	write_sysreg(vcpu->context.regs.pstate, spsr_el2);
	write_sysreg(vcpu->context.regs.pc, elr_el2);

	ret = __guest_enter(vcpu, &hyp_data->hyp_context);

	vcpu->context.regs.pc = read_sysreg(elr_el2);
	vcpu->context.regs.pstate = read_sysreg(spsr_el2);

	__sysreg_save_el1_state(&vcpu->context);

	return ret;
}

void __hyp_exception(u64 type)
{
	GUEST_FAIL("Unexpected hyp exception! type: %lx\n", type);
}
