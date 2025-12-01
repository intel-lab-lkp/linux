/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This contains all required hardware related helper functions for
 * Trace Buffer Extension (TRBE) driver in the coresight framework.
 *
 * Copyright (C) 2020 ARM Ltd.
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#include <linux/acpi.h>
#include <linux/coresight.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>
#include <linux/smp.h>

#include <kunit/test-bug.h>
#include <kunit/visibility.h>

#include "coresight-etm-perf.h"

struct trbe_buf {
	/*
	 * Even though trbe_base represents vmap()
	 * mapped allocated buffer's start address,
	 * it's being as unsigned long for various
	 * arithmetic and comparision operations &
	 * also to be consistent with trbe_write &
	 * trbe_limit sibling pointers.
	 */
	unsigned long trbe_base;
	/* The base programmed into the TRBE */
	unsigned long trbe_hw_base;
	unsigned long trbe_limit;
	unsigned long trbe_write;
	unsigned long trbe_count;
	int nr_pages;
	void **pages;
	bool snapshot;
	struct trbe_cpudata *cpudata;
};

/*
 * TRBE erratum list
 *
 * The errata are defined in arm64 generic cpu_errata framework.
 * Since the errata work arounds could be applied individually
 * to the affected CPUs inside the TRBE driver, we need to know if
 * a given CPU is affected by the erratum. Unlike the other erratum
 * work arounds, TRBE driver needs to check multiple times during
 * a trace session. Thus we need a quicker access to per-CPU
 * errata and not issue costly this_cpu_has_cap() everytime.
 * We keep a set of the affected errata in trbe_cpudata, per TRBE.
 *
 * We rely on the corresponding cpucaps to be defined for a given
 * TRBE erratum. We map the given cpucap into a TRBE internal number
 * to make the tracking of the errata lean.
 *
 * This helps in :
 *   - Not duplicating the detection logic
 *   - Streamlined detection of erratum across the system
 */
#define TRBE_WORKAROUND_OVERWRITE_FILL_MODE	0
#define TRBE_WORKAROUND_WRITE_OUT_OF_RANGE	1
#define TRBE_NEEDS_DRAIN_AFTER_DISABLE		2
#define TRBE_NEEDS_CTXT_SYNC_AFTER_ENABLE	3
#define TRBE_IS_BROKEN				4

static int trbe_errata_cpucaps[] = {
	[TRBE_WORKAROUND_OVERWRITE_FILL_MODE] = ARM64_WORKAROUND_TRBE_OVERWRITE_FILL_MODE,
	[TRBE_WORKAROUND_WRITE_OUT_OF_RANGE] = ARM64_WORKAROUND_TRBE_WRITE_OUT_OF_RANGE,
	[TRBE_NEEDS_DRAIN_AFTER_DISABLE] = ARM64_WORKAROUND_2064142,
	[TRBE_NEEDS_CTXT_SYNC_AFTER_ENABLE] = ARM64_WORKAROUND_2038923,
	[TRBE_IS_BROKEN] = ARM64_WORKAROUND_1902691,
	-1,		/* Sentinel, must be the last entry */
};

/* The total number of listed errata in trbe_errata_cpucaps */
#define TRBE_ERRATA_MAX			(ARRAY_SIZE(trbe_errata_cpucaps) - 1)

/*
 * struct trbe_cpudata: TRBE instance specific data
 * @trbe_flag		- TRBE dirty/access flag support
 * @trbe_hw_align	- Actual TRBE alignment required for TRBPTR_EL1.
 * @trbe_align		- Software alignment used for the TRBPTR_EL1.
 * @cpu			- CPU this TRBE belongs to.
 * @mode		- Mode of current operation. (perf/disabled)
 * @drvdata		- TRBE specific drvdata
 * @errata		- Bit map for the errata on this TRBE.
 */
struct trbe_cpudata {
	bool trbe_flag;
	u64 trbe_hw_align;
	u64 trbe_align;
	int cpu;
	enum cs_mode mode;
	struct trbe_buf *buf;
	struct trbe_drvdata *drvdata;
	DECLARE_BITMAP(errata, TRBE_ERRATA_MAX);
};

static inline bool is_trbe_available(void)
{
	u64 aa64dfr0 = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	unsigned int trbe = cpuid_feature_extract_unsigned_field(aa64dfr0,
								 ID_AA64DFR0_EL1_TraceBuffer_SHIFT);

	return trbe >= ID_AA64DFR0_EL1_TraceBuffer_IMP;
}

static inline bool is_trbe_enabled(void)
{
	u64 trblimitr = read_sysreg_s(SYS_TRBLIMITR_EL1);

	return trblimitr & TRBLIMITR_EL1_E;
}

#define TRBE_EC_OTHERS			0x0
#define TRBE_EC_GP_CHECK_FAULT		0X1e
#define TRBE_EC_BUF_MGMT_IMPL		0x1f
#define TRBE_EC_STAGE1_ABORT		0x24
#define TRBE_EC_STAGE2_ABORT		0x25

static inline int get_trbe_ec(u64 trbsr)
{
	return (trbsr & TRBSR_EL1_EC_MASK) >> TRBSR_EL1_EC_SHIFT;
}

#define TRBE_BSC_NOT_STOPPED 0
#define TRBE_BSC_FILLED      1
#define TRBE_BSC_TRIGGERED   2

static inline int get_trbe_bsc(u64 trbsr)
{
	return (trbsr & TRBSR_EL1_BSC_MASK) >> TRBSR_EL1_BSC_SHIFT;
}

static inline void clr_trbe_irq(void)
{
	u64 trbsr = read_sysreg_s(SYS_TRBSR_EL1);

	trbsr &= ~TRBSR_EL1_IRQ;
	write_sysreg_s(trbsr, SYS_TRBSR_EL1);
}

static inline bool is_trbe_irq(u64 trbsr)
{
	return trbsr & TRBSR_EL1_IRQ;
}

static inline bool is_trbe_trg(u64 trbsr)
{
	return trbsr & TRBSR_EL1_TRG;
}

static inline bool is_trbe_wrap(u64 trbsr)
{
	return trbsr & TRBSR_EL1_WRAP;
}

static inline bool is_trbe_abort(u64 trbsr)
{
	return trbsr & TRBSR_EL1_EA;
}

static inline bool is_trbe_running(u64 trbsr)
{
	return !(trbsr & TRBSR_EL1_S);
}

static inline bool get_trbe_flag_update(u64 trbidr)
{
	return trbidr & TRBIDR_EL1_F;
}

static inline bool is_trbe_programmable(u64 trbidr)
{
	return !(trbidr & TRBIDR_EL1_P);
}

static inline int get_trbe_address_align(u64 trbidr)
{
	return (trbidr & TRBIDR_EL1_Align_MASK) >> TRBIDR_EL1_Align_SHIFT;
}

static inline unsigned long get_trbe_write_pointer(void)
{
	return read_sysreg_s(SYS_TRBPTR_EL1);
}

static inline void set_trbe_write_pointer(unsigned long addr)
{
	WARN_ON(is_trbe_enabled());
	write_sysreg_s(addr, SYS_TRBPTR_EL1);
}

static inline void set_trbe_trigger_count(unsigned long count)
{
	u64 trbsr;

	write_sysreg_s(count, SYS_TRBTRG_EL1);

	/* TRBSR_EL1.TRG has been cleared in clr_trbe_status() */
	if (!count)
		return;

	trbsr = read_sysreg_s(SYS_TRBSR_EL1);
	write_sysreg_s(trbsr | TRBSR_EL1_TRG, SYS_TRBSR_EL1);
}

static inline unsigned long get_trbe_limit_pointer(void)
{
	u64 trblimitr = read_sysreg_s(SYS_TRBLIMITR_EL1);
	unsigned long addr = trblimitr & TRBLIMITR_EL1_LIMIT_MASK;

	WARN_ON(!IS_ALIGNED(addr, PAGE_SIZE));
	return addr;
}

static inline unsigned long get_trbe_base_pointer(void)
{
	u64 trbbaser = read_sysreg_s(SYS_TRBBASER_EL1);
	unsigned long addr = trbbaser & TRBBASER_EL1_BASE_MASK;

	WARN_ON(!IS_ALIGNED(addr, PAGE_SIZE));
	return addr;
}

static inline void set_trbe_base_pointer(unsigned long addr)
{
	WARN_ON(is_trbe_enabled());
	WARN_ON(!IS_ALIGNED(addr, (1UL << TRBBASER_EL1_BASE_SHIFT)));
	WARN_ON(!IS_ALIGNED(addr, PAGE_SIZE));
	write_sysreg_s(addr, SYS_TRBBASER_EL1);
}

#if IS_ENABLED(CONFIG_KUNIT)
DECLARE_STATIC_KEY_FALSE(trbe_trigger_mode_bypass);
unsigned long __trbe_normal_offset(struct perf_output_handle *handle);
u64 __trbe_normal_trigger_count(struct perf_output_handle *handle);
#endif
