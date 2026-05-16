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

void prepare_hyp(void)
{
	write_sysreg(HCR_EL2_E2H | HCR_EL2_RW, hcr_el2);
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
