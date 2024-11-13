/* SPDX-License-Identifier: GPL-2.0 */
#ifndef	__TASK_SHARED_H__
#define	__TASK_SHARED_H__

#include <linux/mm_types.h>
#include <uapi/linux/task_shared.h>

#ifdef CONFIG_TASKSHARED
/*
 * Track user-kernel shared pages referred by mm_struct
 */
struct ushared_pages {
	struct list_head plist;
	struct list_head frlist;
	unsigned long pcount;
};


/*
 * Following is used for cacheline aligned allocations of shared structures
 * within a page.
 */
union task_shared {
	struct task_sharedinfo ts;
	char    s[128];
};

/*
 * Struct to track per page slots
 */
struct ushared_pg {
	struct list_head list;
	struct list_head fr_list;
	struct page *pages[2];
	u64 bitmap; /* free slots */
	int slot_count;
	unsigned long kaddr;
	unsigned long vaddr; /* user address */
	struct vm_special_mapping ushrd_mapping;
};

/*
 * Following struct is referred by struct task_struct, contains mapped address
 * of per thread shared structure allocated.
 */
struct task_ushrd_struct {
	union task_shared *kaddr; /* kernel address */
	union task_shared *uaddr; /* user address */
	struct ushared_pg *upg;
};

extern void task_ushared_free(struct task_struct *t);
extern void mm_ushared_clear(struct mm_struct *mm);
#else	/* !CONFIG_TASKSHARED */
static inline void task_ushared_free(struct task_struct *t)
{
}

static inline void mm_ushared_clear(struct mm_struct *mm)
{
}
#endif /* !CONFIG_TASKSHARED */
#endif /* __TASK_SHARED_H__ */
