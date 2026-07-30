// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Alternate Injection Support (SEV-SNP Guests)
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Melody Wang <huibo.wang@amd.com>
 */

#include <linux/cpumask.h>
#include <linux/cc_platform.h>

#include <asm/apic.h>
#include <asm/sev.h>

#include "local.h"

extern u8 snp_vmpl;

static int svsm_apic_probe(void)
{
	if (!cc_platform_has(CC_ATTR_SNP_ALTERNATE_INJECTION) || !snp_vmpl)
		return 0;

	/* Alternate Injection and Secure AVIC are mutually exclusive */
	if (cc_platform_has(CC_ATTR_SNP_SECURE_AVIC))
		return 0;

	if (!x2apic_mode) {
		pr_err("Alternate Injection in non x2APIC mode impossible. Terminating.\n");
		sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
	}

	pr_info("Alternate Injection SVSM APIC enabled\n");

	return 1;
}

static int svsm_acpi_madt_oem_check(char *oem_id, char *oem_table_id)
{
	return x2apic_enabled() && cc_platform_has(CC_ATTR_SNP_ALTERNATE_INJECTION) && snp_vmpl;
}

static void svsm_apic_msr_write(u32 reg, u32 v)
{
	u32 msr = APIC_BASE_MSR + (reg >> 4);
	struct svsm_call call = {};
	int ret;

	switch (reg) {
	case APIC_ID:
	case APIC_TASKPRI:
	case APIC_PROCPRI:
	case APIC_EOI:
	case APIC_ISR ... APIC_ISR + 0x70:
	case APIC_TMR ... APIC_TMR + 0x70:
	case APIC_IRR ... APIC_IRR + 0x70:
	case APIC_ICR:
	case APIC_SELF_IPI:
		call.rax = SVSM_APIC_CALL(SVSM_APIC_WRITE_REGISTER);
		call.rcx = msr;
		call.rdx = v;

		ret = svsm_do_call(&call);
		if (ret) {
			pr_err("SVSM_APIC_WRITE_REGISTER: 0x%x, error: %d\n", reg, ret);
			sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
		}
		break;
	default:
		pr_err("SVSM_APIC_WRITE_REGISTER 0x%x not supported\n", reg);
		break;
	}
}

static u32 svsm_apic_msr_read(u32 reg)
{
	u32 msr = APIC_BASE_MSR + (reg >> 4);
	struct svsm_call call = {};
	int ret;

	switch (reg) {
	case APIC_ID:
	case APIC_TASKPRI:
	case APIC_PROCPRI:
	case APIC_EOI:
	case APIC_ISR ... APIC_ISR + 0x70:
	case APIC_TMR ... APIC_TMR + 0x70:
	case APIC_IRR ... APIC_IRR + 0x70:
	case APIC_ICR:
	case APIC_SELF_IPI:
		call.rax = SVSM_APIC_CALL(SVSM_APIC_READ_REGISTER);
		call.rcx = msr;

		ret = svsm_do_call(&call);
		if (ret) {
			pr_err("SVSM_APIC_READ_REGISTER: 0x%x, error: %d\n", reg, ret);
			sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
		}
		break;
	default:
		pr_err("SVSM_APIC_READ_REGISTER: 0x%x not supported\n", reg);
		return 0;
	}

	return call.rdx_out;
}

static inline void svsm_apic_msr_eoi(void)
{
	svsm_apic_msr_write(APIC_EOI, APIC_EOI_ACK);
}

static inline u64 svsm_apic_icr_read(void)
{
	u32 reg;
	struct svsm_call call = {};
	int ret;

	reg = APIC_ICR;

	call.rax = SVSM_APIC_CALL(SVSM_APIC_READ_REGISTER);
	call.rcx = APIC_BASE_MSR + (reg >> 4);

	ret = svsm_do_call(&call);
	if (ret) {
		pr_err("svsm_apic_icr_read error: %d\n", ret);
		sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
	}

	return call.rdx_out;
}

static void svsm_apic_icr_write(u32 low, u32 id)
{
	u64 icr_data;
	u32 reg;
	struct svsm_call call = {};
	int ret;

	reg = APIC_ICR;
	icr_data = ((u64)id) << 32 | low;

	call.rax = SVSM_APIC_CALL(SVSM_APIC_WRITE_REGISTER);
	call.rcx = APIC_BASE_MSR + (reg >> 4);
	call.rdx = icr_data;

	ret = svsm_do_call(&call);
	if (ret) {
		pr_err("svsm_apic_icr_write error: %d\n", ret);
		sev_es_terminate(SEV_TERM_SET_GEN, GHCB_SNP_UNSUPPORTED);
	}
}

static void __svsm_apic_send_IPI_dest(unsigned int apicid, int vector, unsigned int dest)
{
	svsm_apic_icr_write(__prepare_ICR(0, vector, dest), apicid);
}

static void svsm_apic_send_IPI(int cpu, int vector)
{
	u32 dest = per_cpu(x86_cpu_to_apicid, cpu);

	__svsm_apic_send_IPI_dest(dest, vector, APIC_DEST_PHYSICAL);
}

static void __svsm_apic_send_IPI_mask(const struct cpumask *mask, int vector, int apic_dest)
{
	unsigned long query_cpu;
	unsigned long this_cpu;

	guard(irqsave)();

	this_cpu = smp_processor_id();
	for_each_cpu(query_cpu, mask) {
		if (apic_dest == APIC_DEST_ALLBUT && this_cpu == query_cpu)
			continue;

		__svsm_apic_send_IPI_dest(per_cpu(x86_cpu_to_apicid, query_cpu),
					  vector, APIC_DEST_PHYSICAL);
	}
}

static void svsm_apic_send_IPI_mask(const struct cpumask *mask, int vector)
{
	__svsm_apic_send_IPI_mask(mask, vector, APIC_DEST_ALLINC);
}

static void svsm_apic_send_IPI_mask_allbutself(const struct cpumask *mask, int vector)
{
	__svsm_apic_send_IPI_mask(mask, vector, APIC_DEST_ALLBUT);
}

static void __svsm_apic_send_IPI_shorthand(int vector, u32 which)
{
	svsm_apic_icr_write(__prepare_ICR(which, vector, 0), 0);
}

static void svsm_apic_send_IPI_allbutself(int vector)
{
	__svsm_apic_send_IPI_shorthand(vector, APIC_DEST_ALLBUT);
}

static void svsm_apic_send_IPI_all(int vector)
{
	__svsm_apic_send_IPI_shorthand(vector, APIC_DEST_ALLINC);
}

static void svsm_apic_send_IPI_self(int vector)
{
	__svsm_apic_send_IPI_shorthand(vector, APIC_DEST_SELF);
}

static u32 svsm_apic_get_apic_id(u32 id)
{
	return id;
}

static struct apic svsm_apic __ro_after_init = {

	.name				= "svsm apic",
	.probe				= svsm_apic_probe,
	.acpi_madt_oem_check		= svsm_acpi_madt_oem_check,

	.dest_mode_logical		= false,

	.disable_esr			= 0,

	.cpu_present_to_apicid		= default_cpu_present_to_apicid,

	.max_apic_id			= UINT_MAX,
	.x2apic_set_max_apicid		= true,
	.get_apic_id			= svsm_apic_get_apic_id,

	.calc_dest_apicid		= apic_default_calc_apicid,

	.send_IPI			= svsm_apic_send_IPI,
	.send_IPI_mask			= svsm_apic_send_IPI_mask,
	.send_IPI_mask_allbutself	= svsm_apic_send_IPI_mask_allbutself,
	.send_IPI_allbutself		= svsm_apic_send_IPI_allbutself,
	.send_IPI_all			= svsm_apic_send_IPI_all,
	.send_IPI_self			= svsm_apic_send_IPI_self,
	.nmi_to_offline_cpu		= true,

	.read				= svsm_apic_msr_read,
	.write				= svsm_apic_msr_write,
	.eoi				= svsm_apic_msr_eoi,
	.icr_read			= svsm_apic_icr_read,
	.icr_write			= svsm_apic_icr_write,

};
apic_driver(svsm_apic);
