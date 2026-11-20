/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#ifndef __ASM_PMUV3_H
#define __ASM_PMUV3_H

#include <asm/kvm_host.h>

#include <asm/cpufeature.h>
#include <asm/sysreg.h>

#define RETURN_READ_PMEVCNTRN(n) \
	return read_sysreg(pmevcntr##n##_el0)
static inline unsigned long read_pmevcntrn(int n)
{
	PMEVN_SWITCH(n, RETURN_READ_PMEVCNTRN);
	return 0;
}

#define WRITE_PMEVCNTRN(n) \
	write_sysreg(val, pmevcntr##n##_el0)
static inline void write_pmevcntrn(int n, unsigned long val)
{
	PMEVN_SWITCH(n, WRITE_PMEVCNTRN);
}

#define WRITE_PMEVTYPERN(n) \
	write_sysreg(val, pmevtyper##n##_el0)
static inline void write_pmevtypern(int n, unsigned long val)
{
	PMEVN_SWITCH(n, WRITE_PMEVTYPERN);
}

#define RETURN_READ_PMEVTYPERN(n) \
	return read_sysreg(pmevtyper##n##_el0)
static inline unsigned long read_pmevtypern(int n)
{
	PMEVN_SWITCH(n, RETURN_READ_PMEVTYPERN);
	return 0;
}

static inline unsigned long read_pmmir(void)
{
	return read_cpuid(PMMIR_EL1);
}

static inline u32 read_pmuver(void)
{
	u64 dfr0 = read_sysreg(id_aa64dfr0_el1);

	return cpuid_feature_extract_unsigned_field(dfr0,
			ID_AA64DFR0_EL1_PMUVer_SHIFT);
}

static inline bool pmuv3_has_icntr(void)
{
	u64 dfr1 = read_sysreg(id_aa64dfr1_el1);

	return !!cpuid_feature_extract_unsigned_field(dfr1,
			ID_AA64DFR1_EL1_PMICNTR_SHIFT);
}

static inline void write_pmcr(u64 val)
{
	write_sysreg(val, pmcr_el0);
}

static inline u64 read_pmcr(void)
{
	return read_sysreg(pmcr_el0);
}

static inline void write_pmselr(u32 val)
{
	write_sysreg(val, pmselr_el0);
}

static inline void write_pmccntr(u64 val)
{
	write_sysreg(val, pmccntr_el0);
}

static inline u64 read_pmccntr(void)
{
	return read_sysreg(pmccntr_el0);
}

static inline void write_pmicntr(u64 val)
{
	write_sysreg_s(val, SYS_PMICNTR_EL0);
}

static inline u64 read_pmicntr(void)
{
	return read_sysreg_s(SYS_PMICNTR_EL0);
}

static inline void write_pmcntenset(u64 val)
{
	write_sysreg(val, pmcntenset_el0);
}

static inline void write_pmcntenclr(u64 val)
{
	write_sysreg(val, pmcntenclr_el0);
}

static inline void write_pmintenset(u64 val)
{
	write_sysreg(val, pmintenset_el1);
}

static inline void write_pmintenclr(u64 val)
{
	write_sysreg(val, pmintenclr_el1);
}

static inline void write_pmccfiltr(u64 val)
{
	write_sysreg(val, pmccfiltr_el0);
}

static inline u64 read_pmccfiltr(void)
{
	return read_sysreg(pmccfiltr_el0);
}

static inline void write_pmicfiltr(u64 val)
{
	write_sysreg_s(val, SYS_PMICFILTR_EL0);
}

static inline u64 read_pmicfiltr(void)
{
	return read_sysreg_s(SYS_PMICFILTR_EL0);
}

static inline void write_pmovsclr(u64 val)
{
	write_sysreg(val, pmovsclr_el0);
}

static inline u64 read_pmovsclr(void)
{
	return read_sysreg(pmovsclr_el0);
}

static inline void write_pmuserenr(u32 val)
{
	write_sysreg(val, pmuserenr_el0);
}

static inline void write_pmuacr(u64 val)
{
	write_sysreg_s(val, SYS_PMUACR_EL1);
}

static inline u64 read_pmceid0(void)
{
	return read_sysreg(pmceid0_el0);
}

static inline u64 read_pmceid1(void)
{
	return read_sysreg(pmceid1_el0);
}

static inline bool pmuv3_implemented(int pmuver)
{
	return !(pmuver == ID_AA64DFR0_EL1_PMUVer_IMP_DEF ||
		 pmuver == ID_AA64DFR0_EL1_PMUVer_NI);
}

/*
 * Whether this CPU's PMU implements a common event, as reported by
 * PMCEID{0,1}: events 0x0000-0x003f in bits [31:0], and from PMUv3p1 events
 * 0x4000-0x403f in bits [63:32], which read as zero before PMUv3p1.
 * Must run on the CPU whose PMU is queried.
 */
static inline bool pmuv3_event_implemented(u16 event)
{
	bool ext = event >= ARMV8_PMUV3_EXT_COMMON_EVENT_BASE;
	u16 id = ext ? event - ARMV8_PMUV3_EXT_COMMON_EVENT_BASE : event;
	u64 pmceid;

	if (id >= ARMV8_PMUV3_MAX_COMMON_EVENTS ||
	    !pmuv3_implemented(read_pmuver()))
		return false;

	pmceid = id < 32 ? read_pmceid0() : read_pmceid1();
	return pmceid & BIT_ULL(id % 32 + (ext ? 32 : 0));
}

static inline bool is_pmuv3p4(int pmuver)
{
	return pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P4;
}

static inline bool is_pmuv3p5(int pmuver)
{
	return pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P5;
}

static inline bool is_pmuv3p9(int pmuver)
{
	return pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P9;
}

#endif
