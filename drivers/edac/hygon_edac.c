// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hygon Family 0x18 memory-controller support for amd64_edac.
 *
 * Vendor-specific UMC addressing, node counts, MCA decode, and family init
 * live here so the core amd64_edac.c AMD paths stay unchanged aside from
 * thin hook calls.
 *
 * Linked into amd64_edac.ko together with amd64_edac_core.o (compiled from
 * amd64_edac.c; see drivers/edac/Makefile).
 */

#include <linux/pci.h>
#include <asm/hygon/node.h>

#include "amd64_edac.h"
#include "hygon_edac.h"

/*
 * TODO: Will move to <asm/hygon/node.h> in Hygon Node RFC next version.
 */
/* Hygon compute dies (CDD) start at DFID 4; IO dies occupy DFIDs 0-3. */
#define HYGON_CDD_DFID_BASE	4

/*
 * Hygon Family 0x18 models 0x4-0x8: UMC config is behind an SMN window derived
 * from the compute-die DFID.
 */
u32 hygon_get_umc_base(struct amd64_pvt *pvt, u8 channel)
{
	u8 df_id = 0;
	int ret;
	u32 base = get_umc_base(channel);

	if (!hygon_f18h_model_in_range(0x4, 0x8))
		return base;

	if (!pvt->F3) {
		amd64_warn("Hygon: no DF misc for MC node %u, using default UMC base\n",
			   pvt->mc_node_id);
		return base;
	}

	ret = hygon_get_dfid(pvt->F3, &df_id);
	if (ret) {
		amd64_warn("Hygon: hygon_get_dfid failed for %s (node %u): %d, using default UMC base\n",
			   pci_name(pvt->F3), pvt->mc_node_id, ret);
		return base;
	}

	/*
	 * UMCs exist only on compute dies (CDD), DFID >= HYGON_CDD_DFID_BASE.
	 * I/O dies use DFID < HYGON_CDD_DFID_BASE; the UMC base offset
	 * calculation must not run on I/O dies.
	 */
	if (df_id < HYGON_CDD_DFID_BASE) {
		amd64_warn("Hygon: DFID %u on %s (node %u) is not a compute die, using default UMC base\n",
			   df_id, pci_name(pvt->F3), pvt->mc_node_id);
		return base;
	}

	return base + (0x80000000U + (0x10000000U * (df_id - HYGON_CDD_DFID_BASE)));
}

/*
 * Hygon Family 0x18 UMC channel is encoded in IPID bits [23:20], unlike AMD
 * which uses bits [31:20].
 */
u8 hygon_get_umc_channel(u64 ipid)
{
	return FIELD_GET(GENMASK_ULL(23, 20), ipid);
}

bool hygon_supports_ddr5(void)
{
	return hygon_f18h_model_in_range(0x4, 0x8);
}

void hygon_per_family_init(struct amd64_pvt *pvt)
{
	if (!is_hygon_f18h())
		return;

	switch (pvt->model) {
	case 0x4:
		pvt->max_mcs = 3;
		break;
	case 0x5:
		pvt->max_mcs = 1;
		break;
	case 0x6:
	case 0x7:
	case 0x8:
		pvt->max_mcs = 2;
		break;
	default:
		break;
	}
}
