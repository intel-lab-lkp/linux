/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KHO_VMALLOC_H
#define _LINUX_KHO_VMALLOC_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kho/abi/vmalloc.h>

struct page;

#ifdef CONFIG_KEXEC_HANDOVER

int kho_preserve_vmalloc(void *ptr, struct kho_vmalloc *preservation);
void kho_unpreserve_vmalloc(struct kho_vmalloc *preservation);
void *kho_restore_vmalloc(const struct kho_vmalloc *preservation);

#else /* CONFIG_KEXEC_HANDOVER */

static inline int kho_preserve_vmalloc(void *ptr,
				       struct kho_vmalloc *preservation)
{
	return -EOPNOTSUPP;
}

static inline void kho_unpreserve_vmalloc(struct kho_vmalloc *preservation) { }

static inline void *kho_restore_vmalloc(const struct kho_vmalloc *preservation)
{
	return NULL;
}

#endif /* CONFIG_KEXEC_HANDOVER */

#endif /* _LINUX_KHO_VMALLOC_H */
