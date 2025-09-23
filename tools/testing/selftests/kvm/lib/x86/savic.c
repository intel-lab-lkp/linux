// SPDX-License-Identifier: GPL-2.0-only

/*
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 */

#include "apic.h"
#include "kvm_util.h"
#include "sev.h"

struct apic_page {
	u8 apic_regs[PAGE_SIZE];
} __packed;

struct guest_apic_page {
	struct apic_page apic_page;
	uint64_t gpa;
	uint64_t hva;
} __aligned(PAGE_SIZE);

struct guest_apic_pages {
	struct guest_apic_page guest_apic_page[KVM_MAX_VCPUS];
};

static struct guest_apic_pages *apic_page_pool;

enum lapic_lvt_entry {
	LVT_TIMER,
	LVT_THERMAL_MONITOR,
	LVT_PERFORMANCE_COUNTER,
	LVT_LINT0,
	LVT_LINT1,
	LVT_ERROR,
	APIC_MAX_NR_LVT_ENTRIES,
};

#define MSR_AMD64_SECURE_AVIC_CONTROL      0xc0010138

#define APIC_LVTx(x) (APIC_LVTT + 0x10 * (x))
#define MSR_AMD64_SECURE_AVIC_EN_BIT       0
#define MSR_AMD64_SECURE_AVIC_ALLOWED_NMI_BIT       1

#define SVM_EXIT_AVIC_UNACCELERATED_ACCESS      0x402

/*
 * Initial pool of guest apic backing page.
 */
void guest_apic_pages_init(struct kvm_vm *vm)
{
	struct guest_apic_pages *g_pages;
	struct guest_apic_page *entry;
	vm_vaddr_t vaddr;
	int i;
	size_t sz = align_up(sizeof(struct guest_apic_pages),
			     vm_guest_mode_params[vm->mode].page_size);

	vaddr = vm_vaddr_alloc(vm, sz, KVM_UTIL_MIN_VADDR);

	g_pages = (struct guest_apic_pages *)addr_gva2hva(vm, vaddr);
	memset(g_pages, 0, sz);

	for (i = 0; i < KVM_MAX_VCPUS; ++i) {
		entry = &g_pages->guest_apic_page[i];
		entry->hva = (uint64_t)entry;
		entry->gpa = (uint64_t)addr_hva2gpa(vm, &entry->apic_page);
	}

	apic_page_pool = (struct guest_apic_pages *)vaddr;
	sync_global_to_guest(vm, apic_page_pool);
}

int savic_nr_pages_required(uint64_t page_size)
{
	return align_up(sizeof(struct guest_apic_pages), page_size) / page_size;
}

/*
 * Enable/disable Secure AVIC in control msr.
 *
 * @apic_page  : Guest APIC backing page for the CPU on which
 *	       this function is called.
 * @enable     : Enable/Disable Secure AVIC.
 * @enable_nmi : Allow host to send NMI to the guest.
 */
void set_savic_control_msr(struct guest_apic_page *apic_page, bool enable, bool enable_nmi)
{
	uint64_t val = apic_page->gpa | BIT_ULL(MSR_AMD64_SECURE_AVIC_EN_BIT);

	if (!enable) {
		wrmsr(MSR_AMD64_SECURE_AVIC_CONTROL, 0);
		return;
	}

	if (enable_nmi)
		val |= BIT_ULL(MSR_AMD64_SECURE_AVIC_ALLOWED_NMI_BIT);

	wrmsr(MSR_AMD64_SECURE_AVIC_CONTROL, val);
}

/*
 * Write APIC reg offset in the guest APIC backing page.
 *
 * @apage : Backing page address.
 * @reg   : APIC reg offset corresponding to the xapic MMIO
 *	  offset.
 * @val   : New value to be set for the APIC reg.
 */
void savic_write_reg(struct guest_apic_page *apic_page, uint32_t reg, uint64_t val)
{
	*(volatile uint64_t *)((uint64_t)apic_page + reg) = val;
}

/*
 * Read APIC reg offset from the guest APIC backing page.
 *
 * @apage : Backing page address.
 * @reg   : APIC reg offset corresponding to the xapic MMIO
 *	  offset.
 *
 * @ret   : APIC register value in the guest APIC backing page.
 */
uint64_t savic_read_reg(struct guest_apic_page *apic_page, uint32_t reg)
{
	return *(volatile uint64_t *)((uint64_t)apic_page + reg);
}

/*
 * Write APIC reg value to hypervisor.
 *
 * @reg   : APIC reg offset corresponding to the xapic MMIO
 *	  offset.
 * @val   : Value to be set for the APIC reg.
 */
void savic_hv_write_reg(uint32_t reg, uint64_t val)
{
	sev_es_pv_msr_rw(APIC_BASE_MSR + (reg >> 4), &val, true);
}

/*
 * Read APIC reg offset from hypervisor.
 *
 * @reg   : APIC reg offset corresponding to the xapic MMIO
 *	  offset.
 *
 * @ret   : APIC register value in the hypervisor's APIC state.
 */
uint64_t savic_hv_read_reg(uint32_t reg)
{
	uint64_t val;

	sev_es_pv_msr_rw(APIC_BASE_MSR + (reg >> 4), &val, false);

	return val;
}

static void savic_init_backing_page(struct guest_apic_page *apic_page, uint32_t apic_id)
{
	uint64_t regval;
	enum lapic_lvt_entry i;

	/* Update APIC ID in the backing page */
	savic_write_reg(apic_page, APIC_ID, apic_id);

	/* Set LVR, LDR, LVT* in backing page from host values */
	regval = savic_hv_read_reg(APIC_LVR);
	savic_write_reg(apic_page, APIC_LVR, regval);

	regval = savic_hv_read_reg(APIC_LDR);
	savic_write_reg(apic_page, APIC_LDR, regval);

	for (i = LVT_THERMAL_MONITOR; i < APIC_MAX_NR_LVT_ENTRIES; i++) {
		regval = savic_hv_read_reg(APIC_LVTx(i));
		savic_write_reg(apic_page, APIC_LVTx(i), regval);
	}

	regval = savic_hv_read_reg(APIC_LVT0);
	savic_write_reg(apic_page, APIC_LVT0, regval);

	regval = savic_hv_read_reg(APIC_LVT1);
	savic_write_reg(apic_page, APIC_LVT1, regval);
}

/*
 * Initialize and enable Secure AVIC on a CPU.
 *
 * @context: Called from x2apic enabled context and Secure AVIC disabled.
 */
void savic_enable(void)
{
	uint64_t savic_ctrl_msr_val, exp_msr_val;
	struct guest_apic_page *apic_page;
	uint32_t apic_id;

	__GUEST_ASSERT(apic_page_pool, "Guest APIC pages pool is not initialized");
	apic_id = x2apic_read_reg(APIC_ID);
	apic_page = &apic_page_pool->guest_apic_page[apic_id];

	savic_init_backing_page(apic_page, apic_id);
	set_savic_control_msr(apic_page, true, true);
	savic_ctrl_msr_val = rdmsr(MSR_AMD64_SECURE_AVIC_CONTROL);
	exp_msr_val = apic_page->gpa | BIT_ULL(MSR_AMD64_SECURE_AVIC_EN_BIT) |
			BIT_ULL(MSR_AMD64_SECURE_AVIC_ALLOWED_NMI_BIT);
	__GUEST_ASSERT(savic_ctrl_msr_val == exp_msr_val,
			"SAVIC Control msr unexpected val : 0x%lx, expected : 0x%lx",
			savic_ctrl_msr_val, exp_msr_val);
}

static bool savic_reg_access_is_trapped(uint32_t reg)
{
	switch (reg) {
	case APIC_ID:
	case APIC_TASKPRI:
	case APIC_EOI:
	case APIC_LDR:
	case APIC_SPIV:
	case APIC_ICR:
	case APIC_ICR2:
	case APIC_LVTT:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
	case APIC_TMICT:
	case APIC_TDCR:
		return true;
	case APIC_LVR:
	case APIC_PROCPRI:
	case APIC_TMR:
	case APIC_IRR ... APIC_IRR + 0x70:
	case APIC_TMCCT:
		return false;
	default:
		return false;
	}
}

static void savic_unaccel_apic_msrs_read(struct guest_apic_page *apic_page,
		uint32_t reg, uint64_t *val)
{
	switch (reg) {
	case APIC_TMICT:
	case APIC_TMCCT:
	case APIC_TDCR:
	case APIC_LVTT:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
		*val = savic_hv_read_reg(reg);
		break;
	default:
		__GUEST_ASSERT(0, "Unexpected unaccelerated read trap for reg: %x\n", reg);
	}
}

static void savic_unaccel_apic_msrs_write(struct guest_apic_page *apic_page,
		uint32_t reg, uint64_t val)
{
	switch (reg) {
	/*
	 * APIC_ID value is in sync between guest apic backing page and
	 * hv.
	 * LVT* registers and APIC timer register updates are propagated to hv.
	 */
	case APIC_ID:
	case APIC_LVTT:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
	case APIC_SPIV:
	case APIC_TMICT:
	case APIC_TMCCT:
	case APIC_TDCR:
		savic_write_reg(apic_page, reg, val);
		savic_hv_write_reg(reg, val);
		break;
	/*
	 * LDR is derived in hv from APIC_ID.
	 * TPR, IRR information is not propagated to hv.
	 */
	case APIC_LDR:
	case APIC_TASKPRI:
	case APIC_IRR:
		savic_write_reg(apic_page, reg, val);
		break;
	/*
	 * EOI write need to be propagated to hv for level-triggered
	 * interrupts.
	 */
	case APIC_EOI:
		savic_hv_write_reg(reg, val);
		break;
	default:
		__GUEST_ASSERT(0, "Write not permitted for reg: %x\n", reg);
	}
}

static void handle_savic_unaccel_access(struct ex_regs *regs)
{
	bool write;;
	uint64_t msr = regs->rcx;
	uint32_t reg = (msr - APIC_BASE_MSR) << 4;
	struct guest_apic_page *apic_page;
	uint64_t low = regs->rax;
	uint64_t high = regs->rdx;
	uint64_t val = 0;

	apic_page = &apic_page_pool->guest_apic_page[x2apic_read_reg(APIC_ID)];

	switch (msr) {
	case APIC_BASE_MSR ... APIC_BASE_MSR + 0xff:
		if (savic_reg_access_is_trapped(reg))
			write = *((uint8_t *)regs->rip - 1) == 0x30;
		else
			write = *((uint8_t *)regs->rip + 1) == 0x30;
		if (write) {
			savic_unaccel_apic_msrs_write(apic_page, reg,
						      high << 32 | low);
		} else {
			savic_unaccel_apic_msrs_read(apic_page, reg, &val);
			regs->rax = val & ((1ULL << 32) - 1);
			regs->rdx = val >> 32;
		}
		if (!savic_reg_access_is_trapped(reg))
			regs->rip += 2;
		break;
	default:
		__GUEST_ASSERT(0, "Unknown unaccelerated msr: %lx\n", msr);
		break;
	}
}

void savic_vc_handler(struct ex_regs *regs)
{
	uint64_t exit_code = regs->error_code;

	switch (exit_code) {
	case SVM_EXIT_AVIC_UNACCELERATED_ACCESS:
		handle_savic_unaccel_access(regs);
		break;
	default:
		sev_es_vc_handler(regs);
		break;
	}
}
