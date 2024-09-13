/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SFRAME_H
#define _LINUX_SFRAME_H

#include <linux/mm_types.h>

struct sframe_file {
	unsigned long sframe_addr, text_start, text_end;
};

struct user_unwind_frame;

#ifdef CONFIG_HAVE_USER_UNWIND_SFRAME

#define INIT_MM_SFRAME .sframe_mt = MTREE_INIT(sframe_mt, 0)

extern void sframe_free_mm(struct mm_struct *mm);

extern int __sframe_add_section(struct sframe_file *file);
extern int sframe_add_section(unsigned long sframe_addr, unsigned long text_start, unsigned long text_end);
extern int sframe_remove_section(unsigned long sframe_addr);
extern int sframe_find(unsigned long ip, struct user_unwind_frame *frame);

static inline bool current_has_sframe(void)
{
	struct mm_struct *mm = current->mm;

	return mm && !mtree_empty(&mm->sframe_mt);
}

#else /* !CONFIG_HAVE_USER_UNWIND_SFRAME */

#define INIT_MM_SFRAME

static inline void sframe_free_mm(struct mm_struct *mm) {}

static inline int __sframe_add_section(struct sframe_file *file) { return -EINVAL; }
static inline int sframe_add_section(unsigned long sframe_addr, unsigned long text_start, unsigned long text_end) { return -EINVAL; }
static inline int sframe_remove_section(unsigned long sframe_addr) { return -EINVAL; }
static inline int sframe_find(unsigned long ip, struct user_unwind_frame *frame) { return -EINVAL; }

static inline bool current_has_sframe(void) { return false; }

#endif /* CONFIG_HAVE_USER_UNWIND_SFRAME */

#endif /* _LINUX_SFRAME_H */
