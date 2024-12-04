// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */
#include <linux/notifier.h>
#include <linux/set_memory.h>
#include <asm/mce.h>
#include <cxlmem.h>
#include "mce.h"

static int cxl_handle_mce(struct notifier_block *nb, unsigned long val,
			  void *data)
{
	struct cxl_memdev_state *mds = container_of(nb, struct cxl_memdev_state,
						    mce_notifier);
	struct cxl_memdev *cxlmd = mds->cxlds.cxlmd;
	struct cxl_port *endpoint = cxlmd->endpoint;
	struct mce *mce = (struct mce *)data;
	u64 spa, spa_alias;
	unsigned long pfn;

	if (!mce || !mce_usable_address(mce))
		return NOTIFY_DONE;

	spa = mce->addr & MCI_ADDR_PHYSADDR;

	pfn = spa >> PAGE_SHIFT;
	if (!pfn_valid(pfn))
		return NOTIFY_DONE;

	spa_alias = cxl_port_get_spa_cache_alias(endpoint, spa);
	if (!spa_alias)
		return NOTIFY_DONE;

	pfn = spa_alias >> PAGE_SHIFT;

	/*
	 * Take down the aliased memory page. The original memory page flagged
	 * by the MCE will be taken cared of by the standard MCE handler.
	 */
	dev_emerg(mds->cxlds.dev, "Offlining aliased SPA address: %#llx\n",
		  spa_alias);
	if (!memory_failure(pfn, 0))
		set_mce_nospec(pfn);

	return NOTIFY_OK;
}

void cxl_register_mce_notifier(struct notifier_block *mce_notifier)
{
	mce_notifier->notifier_call = cxl_handle_mce;
	mce_notifier->priority = MCE_PRIO_UC;
	mce_register_decode_chain(mce_notifier);
}
