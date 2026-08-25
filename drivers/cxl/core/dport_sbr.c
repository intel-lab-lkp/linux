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
 * cxl_region_unbind - take a region out of service ahead of a reset
 * @cxlr: region routed through the CXL Downstream Port being reset
 *
 * Unbind the region driver, which tears down everything built on the region:
 * the dax region device, its dax device and the driver bound to it. An SBR
 * zeroes the downstream bus number, so a region left bound would decode to a
 * device in reset.
 *
 * The memory the region hosts is left as it is. A caller that reaches a live
 * device offlines it first; see cxl_region_disable().
 *
 * Context: process context. Driver unbind sleeps, so this cannot run in atomic
 * context.
 */
static void cxl_region_unbind(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;

	device_release_driver(&cxlr->dev);
	dev_dbg(&cxlr->dev, "%s: region unbound before reset, HPA %pr\n",
		__func__, p->res);
}

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
static int cxl_region_disable(struct cxl_region *cxlr)
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

	cxl_region_unbind(cxlr);
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
static void cxl_region_enable(struct cxl_region *cxlr)
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
 * Collect the regions with a member endpoint routed through @dport_pci, the
 * CXL Downstream Port about to be reset. cxl_rwsem.region keeps the topology
 * stable for the duration of the walk only. Each collected region is pinned
 * with get_device() so the object survives after the lock is dropped, since
 * cxl_region_disable()/cxl_region_enable() run with the rwsem released (they
 * unbind and rebind the region driver). Hence snapshot the set first.
 */
static int cxl_sbr_collect_regions(struct pci_dev *dport_pci,
				   struct xarray *regions)
{
	struct cxl_region_ref *cxl_rr;
	struct cxl_dport *dport;
	unsigned long index;
	int count = 0;
	int rc;

	struct cxl_port *port __free(put_cxl_port) =
		find_cxl_port(&dport_pci->dev, &dport);
	if (!port) {
		pci_dbg(dport_pci, "no CXL port found for reset dport\n");
		return 0;
	}

	guard(rwsem_read)(&cxl_rwsem.region);
	xa_for_each(&port->regions, index, cxl_rr) {
		struct cxl_region *cxlr = cxl_rr->region;
		struct cxl_ep *ep;
		unsigned long ep_index;

		/* Skip unless a region endpoint sits below the reset dport. */
		xa_for_each(&cxl_rr->endpoints, ep_index, ep)
			if (ep->dport == dport)
				break;
		if (!ep) {
			dev_dbg(&cxlr->dev, "%s: no endpoint below %s, region excluded\n",
				__func__, dev_name(dport->dport_dev));
			continue;
		}

		get_device(&cxlr->dev);
		rc = xa_insert(regions, (unsigned long)cxlr, cxlr, GFP_KERNEL);
		if (rc) {
			put_device(&cxlr->dev);
			return rc;
		}
		dev_dbg(&cxlr->dev, "%s: endpoint below %s, region collected\n",
			__func__, dev_name(dport->dport_dev));
		count++;
	}

	dev_dbg(&port->dev, "%d region(s) routed through %s\n", count,
		dev_name(dport->dport_dev));
	return 0;
}

static void cxl_sbr_put_regions(struct xarray *regions)
{
	struct cxl_region *cxlr;
	unsigned long index;

	xa_for_each(regions, index, cxlr)
		put_device(&cxlr->dev);
	xa_destroy(regions);
}

/*
 * The reset cleared the HDM Decoder registers of every CXL component below
 * @dport_pci, so restore them from the settings the driver holds and from
 * @hdm_state, the register fields the driver does not model, saved before the
 * reset. Takes cxl_rwsem.region for read, which cxl_port_recommit_decoders()
 * requires. The caller has already disabled the regions, so nothing reaches the
 * decoders being reprogrammed.
 */
static void cxl_sbr_recommit_decoders(struct pci_dev *dport_pci,
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

/*
 * The HDM decoder control registers the reset is about to clear, held from the
 * disable to the enable of one Downstream Port and indexed by that Port's
 * struct pci_dev, so resets of different Ports do not share an entry.
 */
static DEFINE_XARRAY(cxl_sbr_hdm_state);

static void cxl_sbr_drop_hdm_state(struct pci_dev *dport_pci)
{
	struct xarray *hdm_state;

	hdm_state = xa_erase(&cxl_sbr_hdm_state, (unsigned long)dport_pci);
	if (!hdm_state)
		return;

	cxl_port_put_hdm_state(hdm_state);
	kfree(hdm_state);
}

/*
 * Record the control registers of every port below @dport_pci before the reset
 * clears them. cxl_sbr_enable_regions() consumes the set and drops it.
 */
static int cxl_sbr_save_hdm_state(struct pci_dev *dport_pci)
{
	struct xarray *hdm_state;
	struct cxl_dport *dport;
	int rc;

	struct cxl_port *port __free(put_cxl_port) =
		find_cxl_port(&dport_pci->dev, &dport);
	if (!port)
		return 0;

	hdm_state = kzalloc_obj(*hdm_state);
	if (!hdm_state)
		return -ENOMEM;

	xa_init(hdm_state);

	scoped_guard(rwsem_read, &cxl_rwsem.region)
		rc = cxl_port_save_hdm_state(port, hdm_state);

	if (!rc)
		rc = xa_insert(&cxl_sbr_hdm_state, (unsigned long)dport_pci,
			       hdm_state, GFP_KERNEL);
	if (rc) {
		cxl_port_put_hdm_state(hdm_state);
		kfree(hdm_state);
		return rc;
	}

	return 0;
}

/*
 * Disable the regions routed through the Downstream Port being reset. On
 * failure re-enable the regions already disabled and return the error so the
 * PCI core aborts the reset with the topology unchanged.
 */
static int cxl_sbr_disable_regions(struct pci_dev *dport_pci)
{
	struct cxl_region *cxlr;
	struct xarray regions;
	unsigned long index;
	int rc;

	rc = cxl_sbr_save_hdm_state(dport_pci);
	if (rc)
		return rc;

	xa_init(&regions);

	rc = cxl_sbr_collect_regions(dport_pci, &regions);
	if (rc)
		goto out;

	xa_for_each(&regions, index, cxlr) {
		rc = cxl_region_disable(cxlr);
		if (rc)
			break;
	}

	/*
	 * On failure restore every collected region and return the error so the
	 * PCI core aborts the reset before touching the hardware. Re-enabling a
	 * region left untouched is a no-op, so enabling the whole set also
	 * recovers the region whose offline failed midway.
	 */
	if (rc) {
		dev_dbg(&dport_pci->dev, "%s: disable failed (%d), re-enabling collected regions and aborting reset\n",
			__func__, rc);
		xa_for_each(&regions, index, cxlr)
			cxl_region_enable(cxlr);
	}

out:
	cxl_sbr_put_regions(&regions);
	/* No enable_regions() call follows an aborted reset, so drop the set. */
	if (rc)
		cxl_sbr_drop_hdm_state(dport_pci);
	return rc;
}

/*
 * Unbind the regions routed through the Downstream Port being reset, leaving
 * their memory online. Used on the DPC recovery path, where dpc_reset_link()
 * clears DPC Trigger Status and enters the reset without waiting for the link,
 * so the device may still be unreachable and the page migration that an offline
 * performs would have no device to read from.
 *
 * Unbinding cannot fail, so unlike cxl_sbr_disable_regions() this never aborts
 * the reset. The memory stays online across the reset with no region decoding
 * it; cxl_sbr_enable_regions() reprograms the decoders on the way out.
 */
static void cxl_sbr_unbind_regions(struct pci_dev *dport_pci)
{
	struct cxl_region *cxlr;
	struct xarray regions;
	unsigned long index;

	if (cxl_sbr_save_hdm_state(dport_pci))
		pci_warn(dport_pci, "HDM state not saved, decode will not be restored\n");

	xa_init(&regions);

	cxl_sbr_collect_regions(dport_pci, &regions);

	xa_for_each(&regions, index, cxlr)
		cxl_region_unbind(cxlr);

	cxl_sbr_put_regions(&regions);
}

/*
 * Re-enable the regions disabled by cxl_sbr_disable_regions(). Restore the HDM
 * decode first: a region cannot serve memory through decoders that are not
 * programmed, so its driver must not re-attach before they are.
 */
static void cxl_sbr_enable_regions(struct pci_dev *dport_pci)
{
	struct xarray *hdm_state;
	struct cxl_region *cxlr;
	struct xarray regions;
	unsigned long index;

	xa_init(&regions);

	cxl_sbr_collect_regions(dport_pci, &regions);

	hdm_state = xa_load(&cxl_sbr_hdm_state, (unsigned long)dport_pci);
	if (hdm_state)
		cxl_sbr_recommit_decoders(dport_pci, hdm_state);
	else
		pci_warn(dport_pci, "no saved HDM state, decode not restored\n");

	xa_for_each(&regions, index, cxlr)
		cxl_region_enable(cxlr);

	cxl_sbr_put_regions(&regions);
	cxl_sbr_drop_hdm_state(dport_pci);
}

const struct pci_cxl_sbr_region_ops cxl_sbr_region_ops = {
	.disable_regions = cxl_sbr_disable_regions,
	.unbind_regions = cxl_sbr_unbind_regions,
	.enable_regions = cxl_sbr_enable_regions,
};
