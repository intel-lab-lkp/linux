// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation. */

/*
 * Intel driver to reset bitfix filters when they overflow.
 *
 * Each bitfix filter has limited slots to track corrected errors.
 * These slots can be filled by transient corrected errors (e.g.,
 * bit flips from particle strikes) that don't represent permanent
 * hardware defects.
 *
 * When the filter overflows (yellow status), reset it to reclaim
 * slots occupied by transient corrected errors. If the overflow
 * repeats frequently after reset, it indicates persistent hardware
 * defects that need attention: the system should be scheduled for
 * servicing.
 */
#define pr_fmt(fmt) "bff: " fmt

#include <linux/bits.h>
#include <linux/cacheinfo.h>
#include <linux/cleanup.h>
#include <linux/cpufeature.h>
#include <linux/cpuhplock.h>
#include <linux/device-id/x86_cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/topology.h>
#include <linux/types.h>

#include <asm/cpu_device_id.h>
#include <asm/cpufeatures.h>
#include <asm/intel-family.h>
#include <asm/mce.h>
#include <asm/msr.h>
#include <asm/msr-index.h>

#define NUM_IMH_PER_SKT		2

/* Intel bitfix filter control register defines */
#define MSR_MC0_BFF_CTL		0x000006c0
#define MSR_MCx_BFF_CTL(x)	(MSR_MC0_BFF_CTL + (x))
#define  MCI_BFF_RESET		BIT_ULL(0)

/* Machine check bank scope types */
enum bff_type {
	BFF_NONE = 0,
	BFF_BANK_DCU,
	BFF_BANK_DTLB,
	BFF_BANK_MLC,
	BFF_BANK_CCF,
	BFF_BANK_HSF,
	BFF_BANK_IOCACHE,
};

static const enum bff_type dmr_mcbanks[MAX_NR_BANKS] = {
	[1]	= BFF_BANK_DCU,
	[2]	= BFF_BANK_DTLB,
	[3]	= BFF_BANK_MLC,
	[6]	= BFF_BANK_CCF,
	[13]	= BFF_BANK_HSF,
	[17]	= BFF_BANK_IOCACHE
};

static const struct x86_cpu_id bff_cpu_ids[] __initconst = {
	X86_MATCH_VFM(INTEL_DIAMONDRAPIDS_X, dmr_mcbanks),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, bff_cpu_ids);

static const enum bff_type *bank_types;

/* Diamond Rapids maps APICID[2] to the IMH instance. */
static void bff_set_imh_id(struct mce *mce, unsigned long *id)
{
	int imh_num = (NUM_IMH_PER_SKT * topology_physical_package_id(mce->extcpu)) +
		      ((mce->apicid >> 2) & 0x1);

	*id |= imh_num;
}

static bool bff_set_cache_id(int cpu, int level, unsigned long *id)
{
	int cacheid;

	guard(cpus_read_lock)();

	cacheid = get_cpu_cacheinfo_id(cpu, level);
	if (cacheid == -1) {
		pr_warn("Could not get L%d cache id for CPU %d\n", level, cpu);
		return false;
	}

	*id |= cacheid;

	return true;
}

#define BFF_ID_BANK_SHIFT	16

/*
 * Cache IDs are only unique within a cache level.
 * Include the MCA bank number so each BFF-capable hardware
 * resource has a unique tracking ID.
 */
static unsigned long get_bff_id(struct mce *mce)
{
	unsigned long id = (unsigned long)mce->bank << BFF_ID_BANK_SHIFT;

	switch (bank_types[mce->bank]) {
	case BFF_BANK_DCU:
	case BFF_BANK_DTLB:
		if (!bff_set_cache_id(mce->extcpu, 1, &id))
			return ULONG_MAX;
		break;

	case BFF_BANK_MLC:
		if (!bff_set_cache_id(mce->extcpu, 2, &id))
			return ULONG_MAX;
		break;

	case BFF_BANK_CCF:
		if (!bff_set_cache_id(mce->extcpu, 3, &id))
			return ULONG_MAX;
		break;

	case BFF_BANK_HSF:
	case BFF_BANK_IOCACHE:
		bff_set_imh_id(mce, &id);
		break;
	default:
		return ULONG_MAX;
	}

	return id;
}

static void handle_bff(struct mce *mce)
{
	/* The bitfix filter overflowed, get the target CPU to reset it. */
	if (wrmsrq_on_cpu(mce->extcpu, MSR_MCx_BFF_CTL(mce->bank), MCI_BFF_RESET))
		pr_warn("Failed to reset bitfix filter for CPU %d Bank %d\n", mce->extcpu, mce->bank);

	pr_debug("unique_id = 0x%lx\n", get_bff_id(mce));
}

static int bff_mce_notify(struct notifier_block *nb, unsigned long val, void *data)
{
	struct mce *mce = (struct mce *)data;

	/* TES is undefined for uncorrected errors. */
	if (mce->status & MCI_STATUS_UC)
		return NOTIFY_DONE;

	if (MCI_STATUS_TES(mce->status) != MCI_STATUS_TES_YELLOW)
		return NOTIFY_DONE;

	if (mce->bank >= MAX_NR_BANKS || bank_types[mce->bank] == BFF_NONE)
		return NOTIFY_DONE;

	handle_bff(mce);

	return NOTIFY_DONE;
}

static struct notifier_block bff_notifier = {
	.notifier_call	= bff_mce_notify,
	.priority	= MCE_PRIO_EARLY,
};

static int __init bff_init(void)
{
	const struct x86_cpu_id *m;
	u64 core_caps, mcg_cap;

	if (!cpu_feature_enabled(X86_FEATURE_MCA))
		return -ENODEV;
	rdmsrq(MSR_IA32_MCG_CAP, mcg_cap);
	if (!(mcg_cap & MCG_TES_P))
		return -ENODEV;

	if (!cpu_feature_enabled(X86_FEATURE_CORE_CAPABILITIES))
		return -ENODEV;

	rdmsrq(MSR_IA32_CORE_CAPS, core_caps);
	if (!(core_caps & MSR_IA32_CORE_CAPS_BFF_RESET_DETECT))
		return -ENODEV;

	m = x86_match_cpu(bff_cpu_ids);
	if (!m) {
		pr_info("CPU supports bitfix filter, but driver does not\n");
		return -ENODEV;
	}

	bank_types = (const enum bff_type *)m->driver_data;

	mce_register_decode_chain(&bff_notifier);

	return 0;
}

static void __exit bff_exit(void)
{
	mce_unregister_decode_chain(&bff_notifier);
}

module_init(bff_init);
module_exit(bff_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Intel bitfix filter reset driver");
