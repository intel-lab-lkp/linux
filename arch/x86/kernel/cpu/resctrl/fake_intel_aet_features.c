// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cleanup.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include "fake_intel_aet_features.h"
#include <linux/intel_vsec.h>
#include <linux/resctrl.h>

#include "internal.h"

/* Amount of memory for each fake MMIO space */
#define ENERGY_QWORDS	((576 * 2) + 3)
#define ENERGY_SIZE	(ENERGY_QWORDS * 8)
#define PERF_QWORDS	((576 * 7) + 3)
#define PERF_SIZE	(PERF_QWORDS * 8)

static long pg[4 * ENERGY_QWORDS + 2 * PERF_QWORDS];

/*
 * Fill the fake MMIO space with all different values,
 * all with BIT(63) set to indicate valid entries.
 */
static int __init fill(void)
{
	u64 val = 0;

	for (int i = 0; i < sizeof(pg); i += sizeof(val)) {
		pg[i / sizeof(val)] = BIT_ULL(63) + val;
		val++;
	}
	return 0;
}
device_initcall(fill);

#define PKG_REGION(_entry, _guid, _addr, _size, _pkg, _num_rmids)	\
	[_entry] = { .guid = _guid, .addr = (void __iomem *)_addr, \
		     .num_rmids = _num_rmids, \
		     .size = _size, .plat_info = { .package_id = _pkg }}

/*
 * Set up a fake return for call to:
 *   intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_ENERGY_TELEM);
 * Pretend there are two aggregators on each of the sockets to test
 * the code that sums over multiple aggregators.
 */
static struct pmt_feature_group fake_energy = {
	.count = 4,
	.regions = {
		PKG_REGION(0, 0x26696143, &pg[0 * ENERGY_QWORDS], ENERGY_SIZE, 0, 64),
		PKG_REGION(1, 0x26696143, &pg[1 * ENERGY_QWORDS], ENERGY_SIZE, 0, 64),
		PKG_REGION(2, 0x26696143, &pg[2 * ENERGY_QWORDS], ENERGY_SIZE, 1, 64),
		PKG_REGION(3, 0x26696143, &pg[3 * ENERGY_QWORDS], ENERGY_SIZE, 1, 64)
	}
};

/*
 * Fake return for:
 *   intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_PERF_TELEM);
 */
static struct pmt_feature_group fake_perf = {
	.count = 2,
	.regions = {
		PKG_REGION(0, 0x26557651, &pg[4 * ENERGY_QWORDS + 0 * PERF_QWORDS], PERF_SIZE, 0, 576),
		PKG_REGION(1, 0x26557651, &pg[4 * ENERGY_QWORDS + 1 * PERF_QWORDS], PERF_SIZE, 1, 576)
	}
};

struct pmt_feature_group *
intel_pmt_get_regions_by_feature(enum pmt_feature_id id)
{
	switch (id) {
	case FEATURE_PER_RMID_ENERGY_TELEM:
		return &fake_energy;
	case FEATURE_PER_RMID_PERF_TELEM:
		return &fake_perf;
	default:
		return ERR_PTR(-ENOENT);
	}
	return ERR_PTR(-ENOENT);
}

/*
 * Nothing needed for the "put" function.
 */
void intel_pmt_put_feature_group(struct pmt_feature_group *feature_group)
{
}
