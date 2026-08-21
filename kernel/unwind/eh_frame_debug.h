/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EH_FRAME_DEBUG_H
#define _EH_FRAME_DEBUG_H

#include <linux/eh_frame.h>
#include <linux/mm.h>
#include "eh_frame.h"

#ifdef CONFIG_DYNAMIC_DEBUG

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

#define dbg_sec(fmt, ...)						\
	dbg("%s: " fmt, sec->filename, ##__VA_ARGS__)

static inline void dbg_init(struct eh_frame_section *sec)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	const char *name;

	guard(mmap_read_lock)(mm);
	vma = vma_lookup(mm, sec->eh_frame_hdr_start);
	if (!vma)
		sec->filename = kstrdup("(vma gone???)", GFP_KERNEL_ACCOUNT);
	else if (vma_is_anonymous(vma))
		 sec->filename = kstrdup("(anonymous)", GFP_KERNEL_ACCOUNT);
	else if (vma->vm_file)
		sec->filename = kstrdup_quotable_file(vma->vm_file, GFP_KERNEL_ACCOUNT);
	else if (vma->vm_ops && vma->vm_ops->name && (name = vma->vm_ops->name(vma)))
		sec->filename = kstrdup(name, GFP_KERNEL_ACCOUNT);
	else if (arch_vma_name(vma))
		sec->filename = kstrdup(arch_vma_name(vma), GFP_KERNEL_ACCOUNT);
	else if (!vma->vm_mm)
		sec->filename = kstrdup("(vdso)", GFP_KERNEL_ACCOUNT);
	else
		sec->filename = kstrdup("(vma unknown???)", GFP_KERNEL_ACCOUNT);
}

static inline void dbg_free(struct eh_frame_section *sec)
{
	kfree(sec->filename);
}

#else /* !CONFIG_DYNAMIC_DEBUG */

#define dbg(args...)			no_printk(args)
#define dbg_sec(args...)		no_printk(args)

static inline void dbg_init(struct eh_frame_section *sec) {}
static inline void dbg_free(struct eh_frame_section *sec) {}

#endif /* !CONFIG_DYNAMIC_DEBUG */

#endif /* _EH_FRAME_DEBUG_H */
