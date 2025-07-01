/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SFRAME_H
#define _LINUX_SFRAME_H

#include <linux/mm_types.h>
#include <linux/srcu.h>
#include <linux/unwind_user_types.h>

#ifdef CONFIG_HAVE_UNWIND_USER_SFRAME

struct sframe_section {
	struct rcu_head	rcu;
#ifdef CONFIG_DYNAMIC_DEBUG
	const char	*filename;
#endif
	unsigned long	sframe_start;
	unsigned long	sframe_end;
	unsigned long	text_start;
	unsigned long	text_end;

	unsigned long	fdes_start;
	unsigned long	fres_start;
	unsigned long	fres_end;
	unsigned int	num_fdes;

	signed char	ra_off;
	signed char	fp_off;
};

#define INIT_MM_SFRAME .sframe_mt = MTREE_INIT(sframe_mt, 0),
extern void sframe_free_mm(struct mm_struct *mm);

extern int sframe_add_section(unsigned long sframe_start, unsigned long sframe_end,
			      unsigned long text_start, unsigned long text_end);
extern int sframe_remove_section(unsigned long sframe_addr);
extern int sframe_find(unsigned long ip, struct unwind_user_frame *frame);

static inline bool current_has_sframe(void)
{
	struct mm_struct *mm = current->mm;

	return mm && !mtree_empty(&mm->sframe_mt);
}

#else /* !CONFIG_HAVE_UNWIND_USER_SFRAME */

#define INIT_MM_SFRAME
static inline void sframe_free_mm(struct mm_struct *mm) {}
static inline int sframe_add_section(unsigned long sframe_start, unsigned long sframe_end,
				     unsigned long text_start, unsigned long text_end)
{
	return -ENOSYS;
}
static inline int sframe_remove_section(unsigned long sframe_addr) { return -ENOSYS; }
static inline int sframe_find(unsigned long ip, struct unwind_user_frame *frame) { return -ENOSYS; }
static inline bool current_has_sframe(void) { return false; }

#endif /* CONFIG_HAVE_UNWIND_USER_SFRAME */

#endif /* _LINUX_SFRAME_H */
