// SPDX-License-Identifier: GPL-2.0
/*
 * CXL Private Region - zswap type implementation
 *
 * This file implements the zswap private region type for CXL devices.
 * It handles registration/unregistration of CXL regions as zswap
 * compressed memory targets.
 */

#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/node.h>
#include <linux/zswap.h>
#include <linux/memory_hotplug.h>
#include "../../cxl.h"
#include "../core.h"
#include "private_region.h"

/*
 * CXL zswap region page_allocated callback
 *
 * This callback is invoked by zswap when a page is allocated from a private
 * node to validate that the page is safe to use. For a real compressed memory
 * device, this would check the device's compression ratio and return an error
 * if the page cannot safely store data.
 *
 * Currently this is a placeholder that always succeeds. A real implementation
 * would query the device hardware to determine if sufficient compression
 * headroom exists.
 */
static int cxl_zswap_page_allocated(struct page *page, void *data)
{
	struct cxl_region *cxlr = data;

	/*
	 * TODO: Query the CXL device to check if this page allocation is safe.
	 *
	 * A real compressed memory device would track its compression ratio
	 * and report whether it has headroom to accept new data. If the
	 * compression ratio is too low (device is near capacity), this should
	 * return -ENOSPC to tell zswap to try another node.
	 *
	 * For now, always succeed since we're testing with regular memory.
	 */
	dev_dbg(&cxlr->dev, "page_allocated callback for nid %d\n",
		page_to_nid(page));

	return 0;
}

/*
 * CXL zswap region page_freed callback
 *
 * This callback is invoked when a page from a private node is being freed.
 * We zero the page before returning it to the allocator so that the compressed
 * memory device can reclaim capacity - zeroed pages achieve excellent
 * compression ratios.
 */
static void cxl_zswap_page_freed(struct page *page, void *data)
{
	struct cxl_region *cxlr = data;

	/*
	 * Zero the page to improve the device's compression ratio.
	 * Zeroed pages compress extremely well, reclaiming device capacity.
	 */
	clear_highpage(page);

	dev_dbg(&cxlr->dev, "page_freed callback for nid %d\n",
		page_to_nid(page));
}

/*
 * Unregister a zswap region from the zswap subsystem.
 *
 * This function removes the node from zswap direct nodes and unregisters
 * the private node operations.
 */
void cxl_unregister_zswap_region(struct cxl_region *cxlr)
{
	int nid;

	if (!cxlr->private ||
	    cxlr->private_ops.memtype != NODE_MEM_ZSWAP)
		return;

	if (!cxlr->params.res)
		return;

	nid = phys_to_target_node(cxlr->params.res->start);

	zswap_remove_direct_node(nid);
	node_unregister_private(nid, &cxlr->private_ops);

	dev_dbg(&cxlr->dev, "unregistered zswap region for nid %d\n", nid);
}

/*
 * Register a zswap region with the zswap subsystem.
 *
 * This function sets up the memtype, page_allocated callback, and
 * registers the node with zswap as a direct compression target.
 * The caller is responsible for adding the dax region after this succeeds.
 */
int cxl_register_zswap_region(struct cxl_region *cxlr)
{
	int nid, rc;

	if (!cxlr->private || !cxlr->params.res)
		return -EINVAL;

	nid = phys_to_target_node(cxlr->params.res->start);

	/* Register with node subsystem as zswap memory */
	cxlr->private_ops.memtype = NODE_MEM_ZSWAP;
	cxlr->private_ops.page_allocated = cxl_zswap_page_allocated;
	cxlr->private_ops.page_freed = cxl_zswap_page_freed;
	rc = node_register_private(nid, &cxlr->private_ops);
	if (rc)
		return rc;

	/* Register this node with zswap as a direct compression target */
	zswap_add_direct_node(nid);

	dev_dbg(&cxlr->dev, "registered zswap region for nid %d\n", nid);
	return 0;
}
