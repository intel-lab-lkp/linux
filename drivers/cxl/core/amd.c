// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

#include <linux/prmt.h>
#include <linux/pci.h>

#include "cxlmem.h"
#include "core.h"

#define PCI_DEVICE_ID_AMD_ZEN5_ROOT		0x153e

static const struct pci_device_id zen5_root_port_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_ZEN5_ROOT) },
	{},
};

static int is_zen5_root_port(struct device *dev, void *unused)
{
	if (!dev_is_pci(dev))
		return 0;

	return !!pci_match_id(zen5_root_port_ids, to_pci_dev(dev));
}

static bool is_zen5(struct cxl_port *port)
{
	if (!IS_ENABLED(CONFIG_ACPI_PRMT))
		return false;

	/* To get the CXL root port, find the CXL host bridge first. */
	if (is_cxl_root(port) ||
	    !port->host_bridge ||
	    !is_cxl_root(to_cxl_port(port->dev.parent)))
		return false;

	return !!device_for_each_child(port->host_bridge, NULL,
				       is_zen5_root_port);
}

/*
 * PRM Address Translation - CXL DPA to System Physical Address
 *
 * Reference:
 *
 * AMD Family 1Ah Models 00h–0Fh and Models 10h–1Fh
 * ACPI v6.5 Porting Guide, Publication # 58088
 */

static const guid_t prm_cxl_dpa_spa_guid =
	GUID_INIT(0xee41b397, 0x25d4, 0x452c, 0xad, 0x54, 0x48, 0xc6, 0xe3,
		  0x48, 0x0b, 0x94);

struct prm_cxl_dpa_spa_data {
	u64 dpa;
	u8 reserved;
	u8 devfn;
	u8 bus;
	u8 segment;
	void *out;
} __packed;

static u64 prm_cxl_dpa_spa(struct pci_dev *pci_dev, u64 dpa)
{
	struct prm_cxl_dpa_spa_data data;
	u64 spa;
	int rc;

	data = (struct prm_cxl_dpa_spa_data) {
		.dpa     = dpa,
		.devfn   = pci_dev->devfn,
		.bus     = pci_dev->bus->number,
		.segment = pci_domain_nr(pci_dev->bus),
		.out     = &spa,
	};

	rc = acpi_call_prm_handler(prm_cxl_dpa_spa_guid, &data);
	if (rc) {
		pci_dbg(pci_dev, "failed to get SPA for %#llx: %d\n", dpa, rc);
		return ULLONG_MAX;
	}

	pci_dbg(pci_dev, "PRM address translation: DPA -> SPA: %#llx -> %#llx\n", dpa, spa);

	return spa;
}

static u64 cxl_zen5_to_hpa(struct cxl_decoder *cxld, u64 hpa)
{
	struct cxl_memdev *cxlmd;
	struct pci_dev *pci_dev;
	struct cxl_port *port;
	u64 dpa, base, spa, spa2, len, len2, offset, granularity;
	int ways, pos;

	/*
	 * Nothing to do if base is non-zero and Normalized Addressing
	 * is disabled.
	 */
	if (cxld->hpa_range.start)
		return hpa;

	/* Only translate from endpoint to its parent port. */
	if (!is_endpoint_decoder(&cxld->dev))
		return hpa;

	if (hpa > cxld->hpa_range.end) {
		dev_dbg(&cxld->dev, "hpa addr %#llx out of range %#llx-%#llx\n",
			hpa, cxld->hpa_range.start, cxld->hpa_range.end);
		return ULLONG_MAX;
	}

	/*
	 * If the decoder is already attached, the region's base can
	 * be used.
	 */
	if (cxld->region)
		return cxld->region->params.res->start + hpa;

	port = to_cxl_port(cxld->dev.parent);
	cxlmd = port ? to_cxl_memdev(port->uport_dev) : NULL;
	if (!port || !dev_is_pci(cxlmd->dev.parent)) {
		dev_dbg(&cxld->dev, "No endpoint found: %s, range %#llx-%#llx\n",
			dev_name(cxld->dev.parent), cxld->hpa_range.start,
			cxld->hpa_range.end);
		return ULLONG_MAX;
	}
	pci_dev = to_pci_dev(cxlmd->dev.parent);

	/*
	 * The PRM translates DPA->SPA, but we need HPA->SPA.
	 * Determine the interleaving config first, then calculate the
	 * DPA. Maximum granularity (chunk size) is 16k, minimum is
	 * 256. Calculated with:
	 *
	 *	ways	= hpa_len(SZ_16K) / SZ_16K
	 * 	gran	= (hpa_len(SZ_16K) - hpa_len(SZ_16K - SZ_256) - SZ_256)
	 *                / (ways - 1)
	 *	pos	= (hpa_len(SZ_16K) - ways * SZ_16K) / gran
	 */

	/*
	 * DPA magic:
	 *
	 * Position and granularity are unknown yet, use an always
	 * valid DPA:
	 *
	 * 0xd20000 = 13762560 = 16k * 2 * 3 * 2 * 5 * 7 * 2
	 *
	 * It is divisible by all positions 1 to 8. The DPA is valid
	 * for all positions and granularities.
	 */
#define DPA_MAGIC	0xd20000
	base = prm_cxl_dpa_spa(pci_dev, DPA_MAGIC);
	spa  = prm_cxl_dpa_spa(pci_dev, DPA_MAGIC + SZ_16K);
	spa2 = prm_cxl_dpa_spa(pci_dev, DPA_MAGIC + SZ_16K - SZ_256);

	/* Includes checks to avoid div by zero */
	if (!base || base == ULLONG_MAX || spa == ULLONG_MAX ||
	    spa2 == ULLONG_MAX || spa < base + SZ_16K || spa2 <= base ||
	    (spa > base + SZ_16K && spa - spa2 < SZ_256 * 2)) {
		dev_dbg(&cxld->dev, "Error translating HPA: base %#llx, spa %#llx, spa2 %#llx\n",
			base, spa, spa2);
		return ULLONG_MAX;
	}

	len = spa - base;
	len2 = spa2 - base;

	/* offset = pos * granularity */
	if (len == SZ_16K && len2 == SZ_16K - SZ_256) {
		ways = 1;
		offset = 0;
		granularity = 0;
		pos = 0;
	} else {
		ways = len / SZ_16K;
		offset = spa & (SZ_16K - 1);
		granularity = (len - len2 - SZ_256) / (ways - 1);
		pos = offset / granularity;
	}

	base = base - DPA_MAGIC * ways - pos * granularity;
	spa = base + hpa;

	/*
	 * Check SPA using a PRM call for the closest DPA calculated
	 * for the HPA. If the HPA matches a different interleaving
	 * position other than the decoder's, determine its offset to
	 * adjust the SPA.
	 */

	dpa = (hpa & ~(granularity * ways - 1)) / ways
		+ (hpa & (granularity - 1));
	offset = hpa & (granularity * ways - 1) & ~(granularity - 1);
	offset -= pos * granularity;
	spa2 = prm_cxl_dpa_spa(pci_dev, dpa) + offset;

	dev_dbg(&cxld->dev,
		"address mapping found for %s (dpa -> hpa -> spa): %#llx -> %#llx -> %#llx base: %#llx ways: %d pos: %d granularity: %llu\n",
		pci_name(pci_dev), dpa, hpa, spa, base, ways, pos, granularity);

	if (spa != spa2) {
		dev_dbg(&cxld->dev, "SPA calculation failed: %#llx:%#llx\n",
			spa, spa2);
		return ULLONG_MAX;
	}

	return spa;
}

static void cxl_zen5_init(struct cxl_port *port)
{
	if (!is_zen5(port))
		return;

	port->to_hpa = cxl_zen5_to_hpa;

	dev_dbg(port->host_bridge, "PRM address translation enabled for %s.\n",
		dev_name(&port->dev));
}

void cxl_port_setup_amd(struct cxl_port *port)
{
	cxl_zen5_init(port);
}
