// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure AVIC Support (SEV-SNP Guests)
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Author: Neeraj Upadhyay <Neeraj.Upadhyay@amd.com>
 */

#include <linux/cpumask.h>
#include <linux/cc_platform.h>
#include <linux/percpu-defs.h>
#include <linux/align.h>

#include <asm/apic.h>
#include <asm/sev.h>

#include "local.h"

/* APIC_EILVTn(3) is the last defined APIC register. */
#define NR_APIC_REGS	(APIC_EILVTn(4) >> 2)

struct apic_page {
	union {
		u32	regs[NR_APIC_REGS];
		u8	bytes[PAGE_SIZE];
	};
} __aligned(PAGE_SIZE);

static struct apic_page __percpu *apic_page __ro_after_init;

static int x2apic_savic_acpi_madt_oem_check(char *oem_id, char *oem_table_id)
{
	return x2apic_enabled() && cc_platform_has(CC_ATTR_SNP_SECURE_AVIC);
}

static __always_inline u32 get_reg(unsigned int offset)
{
	return READ_ONCE(this_cpu_ptr(apic_page)->regs[offset >> 2]);
}

static __always_inline void set_reg(unsigned int offset, u32 val)
{
	WRITE_ONCE(this_cpu_ptr(apic_page)->regs[offset >> 2], val);
}

#define SAVIC_ALLOWED_IRR	0x204

static u32 x2apic_savic_read(u32 reg)
{
	/*
	 * When Secure AVIC is enabled, rdmsr/wrmsr of APIC registers
	 * result in VC exception (for non-accelerated register accesses)
	 * with VMEXIT_AVIC_NOACCEL error code. The VC exception handler
	 * can read/write the x2APIC register in the guest APIC backing page.
	 * Since doing this would increase the latency of accessing x2APIC
	 * registers, instead of doing rdmsr/wrmsr based accesses and
	 * handling apic register reads/writes in VC exception, the read()
	 * and write() callbacks directly read/write APIC register from/to
	 * the vCPU APIC backing page.
	 */
	switch (reg) {
	case APIC_LVTT:
	case APIC_TMICT:
	case APIC_TMCCT:
	case APIC_TDCR:
	case APIC_ID:
	case APIC_LVR:
	case APIC_TASKPRI:
	case APIC_ARBPRI:
	case APIC_PROCPRI:
	case APIC_LDR:
	case APIC_SPIV:
	case APIC_ESR:
	case APIC_ICR:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_LVTERR:
	case APIC_EFEAT:
	case APIC_ECTRL:
	case APIC_SEOI:
	case APIC_IER:
	case APIC_EILVTn(0) ... APIC_EILVTn(3):
		return get_reg(reg);
	case APIC_ISR ... APIC_ISR + 0x70:
	case APIC_TMR ... APIC_TMR + 0x70:
		if (WARN_ONCE(!IS_ALIGNED(reg, 16),
			      "APIC reg read offset 0x%x not aligned at 16 bytes", reg))
			return 0;
		return get_reg(reg);
	/* IRR and ALLOWED_IRR offset range */
	case APIC_IRR ... APIC_IRR + 0x74:
		/*
		 * Either aligned at 16 bytes for valid IRR reg offset or a
		 * valid Secure AVIC ALLOWED_IRR offset.
		 */
		if (WARN_ONCE(!(IS_ALIGNED(reg, 16) ||
				IS_ALIGNED(reg - SAVIC_ALLOWED_IRR, 16)),
			      "Misaligned IRR/ALLOWED_IRR APIC reg read offset 0x%x", reg))
			return 0;
		return get_reg(reg);
	default:
		pr_err("Permission denied: read of Secure AVIC reg offset 0x%x\n", reg);
		return 0;
	}
}

#define SAVIC_NMI_REQ		0x278

static void x2apic_savic_write(u32 reg, u32 data)
{
	switch (reg) {
	case APIC_LVTT:
	case APIC_LVT0:
	case APIC_LVT1:
	case APIC_TMICT:
	case APIC_TDCR:
	case APIC_SELF_IPI:
	case APIC_TASKPRI:
	case APIC_EOI:
	case APIC_SPIV:
	case SAVIC_NMI_REQ:
	case APIC_ESR:
	case APIC_ICR:
	case APIC_LVTTHMR:
	case APIC_LVTPC:
	case APIC_LVTERR:
	case APIC_ECTRL:
	case APIC_SEOI:
	case APIC_IER:
	case APIC_EILVTn(0) ... APIC_EILVTn(3):
		set_reg(reg, data);
		break;
	/* ALLOWED_IRR offsets are writable */
	case SAVIC_ALLOWED_IRR ... SAVIC_ALLOWED_IRR + 0x70:
		if (IS_ALIGNED(reg - SAVIC_ALLOWED_IRR, 16)) {
			set_reg(reg, data);
			break;
		}
		fallthrough;
	default:
		pr_err("Permission denied: write to Secure AVIC reg offset 0x%x\n", reg);
	}
}

static void x2apic_savic_send_ipi(int cpu, int vector)
{
	u32 dest = per_cpu(x86_cpu_to_apicid, cpu);

	/* x2apic MSRs are special and need a special fence: */
	weak_wrmsr_fence();
	__x2apic_send_IPI_dest(dest, vector, APIC_DEST_PHYSICAL);
}

static void __send_ipi_mask(const struct cpumask *mask, int vector, bool excl_self)
{
	unsigned long query_cpu;
	unsigned long this_cpu;
	unsigned long flags;

	/* x2apic MSRs are special and need a special fence: */
	weak_wrmsr_fence();

	local_irq_save(flags);

	this_cpu = smp_processor_id();
	for_each_cpu(query_cpu, mask) {
		if (excl_self && this_cpu == query_cpu)
			continue;
		__x2apic_send_IPI_dest(per_cpu(x86_cpu_to_apicid, query_cpu),
				       vector, APIC_DEST_PHYSICAL);
	}
	local_irq_restore(flags);
}

static void x2apic_savic_send_ipi_mask(const struct cpumask *mask, int vector)
{
	__send_ipi_mask(mask, vector, false);
}

static void x2apic_savic_send_ipi_mask_allbutself(const struct cpumask *mask, int vector)
{
	__send_ipi_mask(mask, vector, true);
}

static void init_apic_page(void)
{
	u32 apic_id;

	/*
	 * Before Secure AVIC is enabled, APIC msr reads are intercepted.
	 * APIC_ID msr read returns the value from the Hypervisor.
	 */
	apic_id = native_apic_msr_read(APIC_ID);
	set_reg(APIC_ID, apic_id);
}

static void x2apic_savic_setup(void)
{
	void *backing_page;
	enum es_result ret;
	unsigned long gpa;

	init_apic_page();
	backing_page = this_cpu_ptr(apic_page);
	gpa = __pa(backing_page);

	/*
	 * The NPT entry for a vCPU's APIC backing page must always be
	 * present when the vCPU is running in order for Secure AVIC to
	 * function. A VMEXIT_BUSY is returned on VMRUN and the vCPU cannot
	 * be resumed if the NPT entry for the APIC backing page is not
	 * present. Notify GPA of the vCPU's APIC backing page to the
	 * hypervisor by calling savic_register_gpa(). Before executing
	 * VMRUN, the hypervisor makes use of this information to make sure
	 * the APIC backing page is mapped in NPT.
	 */
	ret = savic_register_gpa(gpa);
	if (ret != ES_OK)
		snp_abort();
}

static int x2apic_savic_probe(void)
{
	if (!cc_platform_has(CC_ATTR_SNP_SECURE_AVIC))
		return 0;

	if (!x2apic_mode) {
		pr_err("Secure AVIC enabled in non x2APIC mode\n");
		snp_abort();
	}

	apic_page = alloc_percpu(struct apic_page);
	if (!apic_page)
		snp_abort();

	return 1;
}

static struct apic apic_x2apic_savic __ro_after_init = {

	.name				= "secure avic x2apic",
	.probe				= x2apic_savic_probe,
	.acpi_madt_oem_check		= x2apic_savic_acpi_madt_oem_check,
	.setup				= x2apic_savic_setup,

	.dest_mode_logical		= false,

	.disable_esr			= 0,

	.cpu_present_to_apicid		= default_cpu_present_to_apicid,

	.max_apic_id			= UINT_MAX,
	.x2apic_set_max_apicid		= true,
	.get_apic_id			= x2apic_get_apic_id,

	.calc_dest_apicid		= apic_default_calc_apicid,

	.send_IPI			= x2apic_savic_send_ipi,
	.send_IPI_mask			= x2apic_savic_send_ipi_mask,
	.send_IPI_mask_allbutself	= x2apic_savic_send_ipi_mask_allbutself,
	.send_IPI_allbutself		= x2apic_send_IPI_allbutself,
	.send_IPI_all			= x2apic_send_IPI_all,
	.send_IPI_self			= x2apic_send_IPI_self,
	.nmi_to_offline_cpu		= true,

	.read				= x2apic_savic_read,
	.write				= x2apic_savic_write,
	.eoi				= native_apic_msr_eoi,
	.icr_read			= native_x2apic_icr_read,
	.icr_write			= native_x2apic_icr_write,
};

apic_driver(apic_x2apic_savic);
