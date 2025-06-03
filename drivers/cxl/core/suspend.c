// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/atomic.h>
#include <linux/export.h>
#include "cxlmem.h"
#include "cxlpci.h"

static atomic_t mem_active;
static atomic_t pci_loaded;

bool cxl_mem_active(void)
{
	if (IS_ENABLED(CONFIG_CXL_MEM))
		return atomic_read(&mem_active) != 0;

	return false;
}

void cxl_mem_active_inc(void)
{
	atomic_inc(&mem_active);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_active_inc, "CXL");

void cxl_mem_active_dec(void)
{
	atomic_dec(&mem_active);
}
EXPORT_SYMBOL_NS_GPL(cxl_mem_active_dec, "CXL");

void mark_cxl_pci_loaded(void)
{
	atomic_inc(&pci_loaded);
}
EXPORT_SYMBOL_NS_GPL(mark_cxl_pci_loaded, "CXL");
