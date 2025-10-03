// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * IOMMU API santaizers and debug
 */

#ifndef __LINUX_IOMMU_DEBUG_H
#define __LINUX_IOMMU_DEBUG_H

#ifdef CONFIG_IOMMU_DEBUG_PAGEALLOC

extern struct page_ext_operations page_iommu_debug_ops;

#endif /* CONFIG_IOMMU_DEBUG_PAGEALLOC */

#endif /* __LINUX_IOMMU_DEBUG_H */
