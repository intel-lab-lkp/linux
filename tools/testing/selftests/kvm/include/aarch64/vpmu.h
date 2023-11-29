/* SPDX-License-Identifier: GPL-2.0 */

#include <kvm_util.h>
#include <perf/arm_pmuv3.h>

#define GICD_BASE_GPA	0x8000000ULL
#define GICR_BASE_GPA	0x80A0000ULL

/* The max number of the PMU event counters (excluding the cycle counter) */
#define ARMV8_PMU_MAX_GENERAL_COUNTERS	(ARMV8_PMU_MAX_COUNTERS - 1)

/* The cycle counter bit position that's common among the PMU registers */
#define ARMV8_PMU_CYCLE_IDX		31

struct vpmu_vm {
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int gic_fd;
};

struct vpmu_vm *__create_vpmu_vm(void *guest_code,
				 void (*init_pmu)(struct vpmu_vm *vm, void *arg),
				 void *arg);

struct vpmu_vm *create_vpmu_vm(void *guest_code);

void destroy_vpmu_vm(struct vpmu_vm *vpmu_vm);

static inline uint64_t get_pmcr_n(uint64_t pmcr)
{
	return (pmcr >> ARMV8_PMU_PMCR_N_SHIFT) & ARMV8_PMU_PMCR_N_MASK;
}

static inline void set_pmcr_n(uint64_t *pmcr, uint64_t pmcr_n)
{
	*pmcr = *pmcr & ~(ARMV8_PMU_PMCR_N_MASK << ARMV8_PMU_PMCR_N_SHIFT);
	*pmcr |= (pmcr_n << ARMV8_PMU_PMCR_N_SHIFT);
}

static inline uint64_t get_counters_mask(uint64_t n)
{
	uint64_t mask = BIT(ARMV8_PMU_CYCLE_IDX);

	if (n)
		mask |= GENMASK(n - 1, 0);
	return mask;
}

/* Read PMEVTCNTR<n>_EL0 through PMXEVCNTR_EL0 */
static inline unsigned long read_sel_evcntr(int sel)
{
	write_sysreg(sel, pmselr_el0);
	isb();
	return read_sysreg(pmxevcntr_el0);
}

/* Write PMEVTCNTR<n>_EL0 through PMXEVCNTR_EL0 */
static inline void write_sel_evcntr(int sel, unsigned long val)
{
	write_sysreg(sel, pmselr_el0);
	isb();
	write_sysreg(val, pmxevcntr_el0);
	isb();
}

/* Read PMEVTYPER<n>_EL0 through PMXEVTYPER_EL0 */
static inline unsigned long read_sel_evtyper(int sel)
{
	write_sysreg(sel, pmselr_el0);
	isb();
	return read_sysreg(pmxevtyper_el0);
}

/* Write PMEVTYPER<n>_EL0 through PMXEVTYPER_EL0 */
static inline void write_sel_evtyper(int sel, unsigned long val)
{
	write_sysreg(sel, pmselr_el0);
	isb();
	write_sysreg(val, pmxevtyper_el0);
	isb();
}

static inline void enable_counter(int idx)
{
	write_sysreg(BIT(idx), pmcntenset_el0);
	isb();
}

static inline void disable_counter(int idx)
{
	write_sysreg(BIT(idx), pmcntenclr_el0);
	isb();
}

static inline void pmu_disable_reset(void)
{
	uint64_t pmcr = read_sysreg(pmcr_el0);

	/* Reset all counters, disabling them */
	pmcr &= ~ARMV8_PMU_PMCR_E;
	write_sysreg(pmcr | ARMV8_PMU_PMCR_P, pmcr_el0);
	isb();
}

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
	isb();
}

#define READ_PMEVTYPERN(n) \
	return read_sysreg(pmevtyper##n##_el0)
static inline unsigned long read_pmevtypern(int n)
{
	PMEVN_SWITCH(n, READ_PMEVTYPERN);
	return 0;
}

#define WRITE_PMEVTYPERN(n) \
	write_sysreg(val, pmevtyper##n##_el0)
static inline void write_pmevtypern(int n, unsigned long val)
{
	PMEVN_SWITCH(n, WRITE_PMEVTYPERN);
	isb();
}
