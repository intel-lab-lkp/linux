/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EH_FRAME_H
#define _LINUX_EH_FRAME_H

#include <linux/mm_types.h>
#include <linux/srcu.h>
#include <linux/unwind_user_types.h>

#ifdef CONFIG_HAVE_UNWIND_USER_EH_FRAME

struct eh_frame_section {
	struct rcu_head	rcu;

	unsigned long	eh_frame_hdr_start;
	unsigned long	eh_frame_hdr_end;
	unsigned long	text_start;
	unsigned long	text_end;

	/* .eh_frame_hdr information */
	unsigned long	eh_frame_start;
	unsigned long	eh_frame_vma_end;
	unsigned long	binary_search_table_start;
	unsigned long	binary_search_table_end;
	unsigned long	fde_count;
	u8		binary_search_table_enc;
};

#define EH_FRAME_MT_FLAGS (MT_FLAGS_USE_RCU)

#define INIT_MM_EH_FRAME .eh_frame_mt = MTREE_INIT(eh_frame_mt, EH_FRAME_MT_FLAGS),
extern void eh_frame_free_mm(struct mm_struct *mm);

extern int eh_frame_add_section(unsigned long eh_frame_hdr_start,
				unsigned long eh_frame_hdr_end,
				unsigned long text_start,
				unsigned long text_end);
extern int eh_frame_remove_section(unsigned long eh_frame_hdr_start);
extern int eh_frame_find(unsigned long ip, struct unwind_user_frame *frame);

static inline bool current_has_eh_frame(void)
{
	struct mm_struct *mm = current->mm;

	return mm && !mtree_empty(&mm->eh_frame_mt);
}

#else /* !CONFIG_HAVE_UNWIND_USER_EH_FRAME */

#define INIT_MM_EH_FRAME
static inline void eh_frame_free_mm(struct mm_struct *mm) {}

static inline int eh_frame_add_section(unsigned long eh_frame_hdr_start,
				       unsigned long eh_frame_hdr_end,
				       unsigned long text_start,
				       unsigned long text_end)
{
	return -ENOSYS;
}

static inline int eh_frame_remove_section(unsigned long eh_frame_hdr_start)
{
	return -ENOSYS;
}

static inline int eh_frame_find(unsigned long ip, struct unwind_user_frame *frame)
{
	return -ENOSYS;
}

static inline bool current_has_eh_frame(void) { return false; }

#endif /* CONFIG_HAVE_UNWIND_USER_EH_FRAME */

#endif /* _LINUX_EH_FRAME_H */
