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
