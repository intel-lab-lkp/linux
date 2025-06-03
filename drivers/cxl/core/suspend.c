// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/wait.h>
#include "cxlmem.h"
#include "cxlpci.h"

static atomic_t mem_active;
static atomic_t pci_loaded;

static DECLARE_WAIT_QUEUE_HEAD(cxl_wait_queue);

bool cxl_mem_active(void)
{
	if (IS_ENABLED(CONFIG_CXL_MEM))
		return atomic_read(&mem_active) != 0;

	return false;
}

void cxl_mem_active_inc(void)
{
	atomic_inc(&mem_active);
	wake_up(&cxl_wait_queue);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_active_inc, "CXL");

void cxl_mem_active_dec(void)
{
	atomic_dec(&mem_active);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_active_dec, "CXL");

static bool cxl_pci_loaded(void)
{
	if (IS_ENABLED(CONFIG_CXL_PCI))
		return atomic_read(&pci_loaded) != 0;

	return false;
}

void mark_cxl_pci_loaded(void)
{
	atomic_inc(&pci_loaded);
	wake_up(&cxl_wait_queue);
}
EXPORT_SYMBOL_NS_GPL(mark_cxl_pci_loaded, "CXL");

void cxl_wait_for_pci_mem(void)
{
	if (!wait_event_timeout(cxl_wait_queue, cxl_pci_loaded() &&
				cxl_mem_active(), 30 * HZ))
		pr_debug("Timeout waiting for cxl_pci or cxl_mem probing\n");
}
EXPORT_SYMBOL_NS_GPL(cxl_wait_for_pci_mem, "CXL");
