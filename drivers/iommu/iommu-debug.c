// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * IOMMU API santaizers and debug
 */
#include <linux/atomic.h>
#include <linux/iommu-debug.h>
#include <linux/kernel.h>
#include <linux/page_ext.h>

static bool needed;

struct iommu_debug_metadate {
	atomic_t ref;
};

static __init bool need_iommu_debug(void)
{
	return needed;
}

struct page_ext_operations page_iommu_debug_ops = {
	.size = sizeof(struct iommu_debug_metadate),
	.need = need_iommu_debug,
};

static int __init iommu_debug_pagealloc(char *str)
{
	return kstrtobool(str, &needed);
}
early_param("iommu.debug_pagealloc", iommu_debug_pagealloc);
