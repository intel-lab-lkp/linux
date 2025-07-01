/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SFRAME_DEBUG_H
#define _SFRAME_DEBUG_H

#include <linux/sframe.h>
#include "sframe.h"

#ifdef CONFIG_DYNAMIC_DEBUG

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

#define dbg_sec(fmt, ...)						\
	dbg("%s: " fmt, sec->filename, ##__VA_ARGS__)

static __always_inline void dbg_print_header(struct sframe_section *sec)
{
	unsigned long fdes_end;

	fdes_end = sec->fdes_start + (sec->num_fdes * sizeof(struct sframe_fde));

	dbg_sec("SEC: sframe:0x%lx-0x%lx text:0x%lx-0x%lx "
		"fdes:0x%lx-0x%lx fres:0x%lx-0x%lx "
		"ra_off:%d fp_off:%d\n",
		sec->sframe_start, sec->sframe_end, sec->text_start, sec->text_end,
		sec->fdes_start, fdes_end, sec->fres_start, sec->fres_end,
		sec->ra_off, sec->fp_off);
}

static inline void dbg_init(struct sframe_section *sec)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	guard(mmap_read_lock)(mm);
	vma = vma_lookup(mm, sec->sframe_start);
	if (!vma)
		sec->filename = kstrdup("(vma gone???)", GFP_KERNEL);
	else if (vma->vm_file)
		sec->filename = kstrdup_quotable_file(vma->vm_file, GFP_KERNEL);
	else if (vma->vm_ops && vma->vm_ops->name)
		sec->filename = kstrdup(vma->vm_ops->name(vma), GFP_KERNEL);
	else if (arch_vma_name(vma))
		sec->filename = kstrdup(arch_vma_name(vma), GFP_KERNEL);
	else if (!vma->vm_mm)
		sec->filename = kstrdup("(vdso)", GFP_KERNEL);
	else
		sec->filename = kstrdup("(anonymous)", GFP_KERNEL);
}

static inline void dbg_free(struct sframe_section *sec)
{
	kfree(sec->filename);
}

#else /* !CONFIG_DYNAMIC_DEBUG */

#define dbg(args...)			no_printk(args)
#define dbg_sec(args...	)		no_printk(args)

static inline void dbg_print_header(struct sframe_section *sec) {}

static inline void dbg_init(struct sframe_section *sec) {}
static inline void dbg_free(struct sframe_section *sec) {}

#endif /* !CONFIG_DYNAMIC_DEBUG */

#endif /* _SFRAME_DEBUG_H */
