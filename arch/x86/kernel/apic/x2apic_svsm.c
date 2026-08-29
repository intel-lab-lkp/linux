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

static int svsm_apic_probe(void)
{
	if (cc_platform_has(CC_ATTR_SNP_ALTERNATE_INJECTION) && !snp_vmpl) {
		pr_err("Alternate Injection in VMPL0 impossible. Terminating.\n");
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_ALT_INJ_FAIL);
	}

	if (!cc_platform_has(CC_ATTR_SNP_ALTERNATE_INJECTION))
		return 0;

	if (!x2apic_mode) {
		pr_err("Alternate Injection in non x2APIC mode impossible. Terminating.\n");
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_ALT_INJ_FAIL);
	}

	pr_info("Alternate Injection SVSM APIC enabled\n");

	return 1;
}

static int svsm_acpi_madt_oem_check(char *oem_id, char *oem_table_id)
{
	return x2apic_enabled() && cc_platform_has(CC_ATTR_SNP_ALTERNATE_INJECTION) && snp_vmpl;
}

static u32 __svsm_apic_msr_rw(u32 reg, u32 v, bool write)
{
	u32 msr = APIC_BASE_MSR + (reg >> 4);
	struct svsm_call call = {};
	const char *call_reg_str;
	unsigned int call_reg;
	int ret;

	call_reg = write ? SVSM_APIC_WRITE_REGISTER
			 : SVSM_APIC_READ_REGISTER;

	call_reg_str = write ? "SVSM_APIC_WRITE_REGISTER"
			     : "SVSM_APIC_READ_REGISTER";

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
		call.rax = SVSM_APIC_CALL(call_reg);
		call.rcx = msr;
		call.rdx = v;

		ret = svsm_perform_call_protocol(&call);
		if (ret) { 
			pr_err("%s: 0x%x, error: %d\n", call_reg_str, reg, ret);
			sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_ALT_INJ_FAIL);
		}
		break;
	default:
		if (write) {
			sev_apic_ghcb_msr_write(reg, v);
		} else {
			return sev_apic_ghcb_msr_read(reg);
		}
	}

	return call.rdx_out;
}

static void svsm_apic_msr_write(u32 reg, u32 v)
{
	__svsm_apic_msr_rw(reg, v, true);
}


static u32 svsm_apic_msr_read(u32 reg)
{
	return __svsm_apic_msr_rw(reg, 0, false);
}

static inline void svsm_apic_msr_eoi(void)
{
	svsm_apic_msr_write(APIC_EOI, APIC_EOI_ACK);
}

static inline u64 svsm_apic_icr_read(void)
{
	struct svsm_call call = {};
	u32 reg;
	int ret;

	reg = APIC_ICR;

	call.rax = SVSM_APIC_CALL(SVSM_APIC_READ_REGISTER);
	call.rcx = APIC_BASE_MSR + (reg >> 4);

	ret = svsm_perform_call_protocol(&call);
	if (ret) {
		pr_err("svsm_apic_icr_read error: %d\n", ret);
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_ALT_INJ_FAIL);
	}

	return call.rdx_out;
}

static void svsm_apic_icr_write(u32 low, u32 id)
{
	struct svsm_call call = {};
	u64 icr_data;
	u32 reg;
	int ret;

	reg = APIC_ICR;
	icr_data = ((u64)id) << 32 | low;

	call.rax = SVSM_APIC_CALL(SVSM_APIC_WRITE_REGISTER);
	call.rcx = APIC_BASE_MSR + (reg >> 4);
	call.rdx = icr_data;

	ret = svsm_perform_call_protocol(&call);
	if (ret) {
		pr_err("svsm_apic_icr_write error: %d\n", ret);
		sev_es_terminate(SEV_TERM_SET_LINUX, GHCB_TERM_ALT_INJ_FAIL);
	}
}

static void svsm_apic_send_IPI(int cpu, int vector)
{
	u32 dest = per_cpu(x86_cpu_to_apicid, cpu);

	svsm_apic_icr_write(__prepare_ICR(0, vector, APIC_DEST_PHYSICAL), dest);
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

		svsm_apic_send_IPI(query_cpu, vector);
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
