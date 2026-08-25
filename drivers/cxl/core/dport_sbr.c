// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation. */

#include <linux/memregion.h>
#include <linux/memory_hotplug.h>
#include <linux/memory.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <cxl.h>
#include "core.h"

/*
 * cxl_region_disable - make a region inactive ahead of a Secondary Bus Reset
 * @cxlr: region routed through the CXL Downstream Port being reset
 *
 * Offline the memory blocks the region owns and unbind its driver. An SBR
 * zeroes the downstream bus number, so a region left live as System RAM would
 * be accessed while the device is in reset. On offline failure return the error
 * so the caller aborts the reset; the memory is never force-removed.
 *
 * Context: process context. Offlining and driver unbind sleep and take the
 * memory hotplug lock, so this cannot run in atomic context.
 */
int cxl_region_disable(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	unsigned long block_size;
	u64 start, end;
	int rc;

	/*
	 * Per CXL r4.0 sec 9.13.1 an Interleave Set has a Base HPA and a Size
	 * that are multiples of 256 MB, while a memory block spans up to 2 GB.
	 * A block overlapping either end of the range therefore also covers
	 * memory outside this region, so round the range inward to block
	 * granularity as dax_kmem did when it onlined the range. Offlining a
	 * straddling block would migrate pages that the reset does not affect.
	 */
	block_size = memory_block_size_bytes();
	start = ALIGN(p->res->start, block_size);
	end = ALIGN_DOWN(p->res->end + 1, block_size);
	if (start >= end) {
		dev_dbg(&cxlr->dev, "%s: HPA %pr spans no whole memory block, no System RAM to offline\n",
			__func__, p->res);
	} else {
		rc = cxl_offline_memory(start, end - start);
		if (rc) {
			dev_warn(&cxlr->dev, "offline System RAM failed before reset: %d\n",
				 rc);
			return rc;
		}
	}

	rc = cxl_region_invalidate_memregion(cxlr);
	if (rc) {
		dev_warn(&cxlr->dev, "CPU cache invalidate failed before reset: %d\n",
			 rc);
		return rc;
	}

	device_release_driver(&cxlr->dev);
	dev_dbg(&cxlr->dev, "%s: System RAM offline, region disabled before reset, HPA %pr\n",
		__func__, p->res);

	return 0;
}

/*
 * cxl_region_enable - restore a region after a Secondary Bus Reset
 * @cxlr: region disabled by cxl_region_disable() before the reset
 *
 * Rebind the region driver. The System RAM is left offline; bringing it back
 * online is a separate administrative step.
 */
void cxl_region_enable(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;

	if (device_attach(&cxlr->dev) < 0) {
		dev_dbg(&cxlr->dev, "driver re-attach failed after reset\n");
		return;
	}

	dev_dbg(&cxlr->dev, "%s: region re-enabled after reset, HPA %pr, IW %d, IG %d\n",
		__func__, p->res, p->interleave_ways, p->interleave_granularity);
}

/*
 * The reset cleared the HDM Decoder registers of every CXL component below
 * @dport_pci, so restore them from the settings the driver holds and from
 * @hdm_state, the register fields the driver does not model, saved before the
 * reset. Takes cxl_rwsem.region for read, which cxl_port_recommit_decoders()
 * requires. The caller has already disabled the regions, so nothing reaches the
 * decoders being reprogrammed.
 */
void cxl_sbr_recommit_decoders(struct pci_dev *dport_pci,
			       struct xarray *hdm_state)
{
	struct cxl_dport *dport;
	int rc;

	struct cxl_port *port __free(put_cxl_port) =
		find_cxl_port(&dport_pci->dev, &dport);
	if (!port) {
		pci_dbg(dport_pci, "no CXL port owns this Downstream Port\n");
		return;
	}

	pci_dbg(dport_pci, "restoring HDM decode below %s\n", dev_name(&port->dev));

	guard(rwsem_read)(&cxl_rwsem.region);
	rc = cxl_port_recommit_decoders(port, hdm_state);
	if (rc)
		pci_warn(dport_pci, "HDM decode restore failed: %d\n", rc);
}
