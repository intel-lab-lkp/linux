// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure Encrypted Virtualization (SEV) firmware upload API
 */

#include <linux/firmware.h>
#include <linux/psp-sev.h>

#include <asm/sev.h>

#include "sev-dev.h"

/*
 * After a gctx is created, it is used by snp_launch_start before getting
 * bound to an asid. The launch protocol allocates an asid before creating a
 * matching gctx page, so there should never be more unbound gctx pages than
 * there are possible SNP asids.
 *
 * The unbound gctx pages must be updated after executing DOWNLOAD_FIRMWARE_EX
 * and before committing the firmware.
 */
static void snp_gctx_create_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_addr *gctx_create = data;

	/* This condition should never happen, but is needed for memory safety. */
	if (sev->snp_unbound_gctx_end >= sev->last_snp_asid) {
		dev_warn(sev->dev, "Too many unbound SNP GCTX pages to track\n");
		return;
	}

	sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end] = gctx_create->address;
	sev->snp_unbound_gctx_end++;
}

/*
 * PREREQUISITE: The snp_activate command was successful, meaning the asid
 * is in the acceptable range 1..sev->last_snp_asid.
 *
 * The gctx_paddr must be in the unbound gctx buffer.
 */
static void snp_activate_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_activate *data_activate = data;

	sev->snp_asid_to_gctx_pages_map[data_activate->asid] = data_activate->gctx_paddr;

	for (int i = 0; i < sev->snp_unbound_gctx_end; i++) {
		if (sev->snp_unbound_gctx_pages[i] == data_activate->gctx_paddr) {
			/*
			 * Swap the last unbound gctx page with the now-bound
			 * gctx page to shrink the buffer.
			 */
			sev->snp_unbound_gctx_end--;
			sev->snp_unbound_gctx_pages[i] =
				sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end];
			sev->snp_unbound_gctx_pages[sev->snp_unbound_gctx_end] = 0;
			break;
		}
	}
}

static void snp_decommission_track_locked(struct sev_device *sev, void *data)
{
	struct sev_data_snp_addr *data_decommission = data;

	for (int i = 1; i <= sev->last_snp_asid; i++) {
		if (sev->snp_asid_to_gctx_pages_map[i] == data_decommission->address) {
			sev->snp_asid_to_gctx_pages_map[i] = 0;
			break;
		}
	}
}

void snp_cmd_bookkeeping_locked(int cmd, struct sev_device *sev, void *data)
{
	if (!sev->snp_asid_to_gctx_pages_map)
		return;

	switch (cmd) {
	case SEV_CMD_SNP_GCTX_CREATE:
		snp_gctx_create_track_locked(sev, data);
		break;
	case SEV_CMD_SNP_ACTIVATE:
		snp_activate_track_locked(sev, data);
		break;
	case SEV_CMD_SNP_DECOMMISSION:
		snp_decommission_track_locked(sev, data);
		break;
	default:
		break;
	}
}

int sev_snp_platform_init_firmware_upload(struct sev_device *sev)
{
	u32 max_snp_asid;

	/*
	 * cpuid_edx(0x8000001f) is the minimum SEV ASID, hence the exclusive
	 * maximum SEV-SNP ASID. Save the inclusive maximum to avoid confusing
	 * logic elsewhere.
	 */
	max_snp_asid = cpuid_edx(0x8000001f);
	sev->last_snp_asid = max_snp_asid - 1;
	if (sev->last_snp_asid) {
		sev->snp_asid_to_gctx_pages_map = devm_kmalloc_array(
			sev->dev, max_snp_asid * 2, sizeof(u64), GFP_KERNEL | __GFP_ZERO);
		sev->snp_unbound_gctx_pages = &sev->snp_asid_to_gctx_pages_map[max_snp_asid];
		if (!sev->snp_asid_to_gctx_pages_map) {
			dev_err(sev->dev,
				"SEV-SNP: snp_asid_to_gctx_pages_map memory allocation failed\n");
			return -ENOMEM;
		}
	}
	return 0;
}
