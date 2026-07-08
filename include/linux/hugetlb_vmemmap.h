/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HUGETLB_VMEMMAP_H
#define _LINUX_HUGETLB_VMEMMAP_H

#include <linux/types.h>

#ifdef CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP

bool hugetlb_vmemmap_optimization_try_disable(void);

#else /* CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP */

static inline bool hugetlb_vmemmap_optimization_try_disable(void)
{
	return true;
}

#endif

#endif /* _LINUX_HUGETLB_VMEMMAP_H */
