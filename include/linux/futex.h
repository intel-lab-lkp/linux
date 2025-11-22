/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/mm_types.h>
#include <linux/compat.h>

#include <uapi/linux/futex.h>

struct inode;
struct task_struct;

/*
 * Futexes are matched on equal values of this key.
 * The key type depends on whether it's a shared or private mapping.
 * Don't rearrange members without looking at hash_futex().
 *
 * offset is aligned to a multiple of sizeof(u32) (== 4) by definition.
 * We use the two low order bits of offset to tell what is the kind of key :
 *  00 : Private process futex (PTHREAD_PROCESS_PRIVATE)
 *       (no reference on an inode or mm)
 *  01 : Shared futex (PTHREAD_PROCESS_SHARED)
 *	mapped on a file (reference on the underlying inode)
 *  10 : Shared futex (PTHREAD_PROCESS_SHARED)
 *       (but private mapping on an mm, and reference taken on it)
*/

#define FUT_OFF_INODE    1 /* We set bit 0 if key has a reference on inode */
#define FUT_OFF_MMSHARED 2 /* We set bit 1 if key has a reference on mm */

union futex_key {
	struct {
		u64 i_seq;
		unsigned long pgoff;
		unsigned int offset;
		/* unsigned int node; */
	} shared;
	struct {
		union {
			struct mm_struct *mm;
			u64 __tmp;
		};
		unsigned long address;
		unsigned int offset;
		/* unsigned int node; */
	} private;
	struct {
		u64 ptr;
		unsigned long word;
		unsigned int offset;
		unsigned int node;	/* NOT hashed! */
	} both;
};

#define FUTEX_KEY_INIT (union futex_key) { .both = { .ptr = 0ULL } }

#ifdef CONFIG_FUTEX
enum {
	FUTEX_STATE_OK,
	FUTEX_STATE_EXITING,
	FUTEX_STATE_DEAD,
};

#define FUTEX_ROBUST_LIST_NATIVE_IDX	0
#define FUTEX_ROBUST_LIST_COMPAT_IDX	1
#define FUTEX_ROBUST_LIST2_IDX		2
#define FUTEX_ROBUST_LISTS_PER_USER	8
#define FUTEX_ROBUST_LIST2_MAX_IDX	(FUTEX_ROBUST_LIST2_IDX + FUTEX_ROBUST_LISTS_PER_USER)

/*
 * List entries without _32BIT flag are using the native machine size
 */
#define FUTEX_ROBUST_LIST_ENTRY_INUSE	0x1UL
#define FUTEX_ROBUST_LIST_ENTRY_32BIT	0x2UL
#define FUTEX_ROBUST_LIST_ENTRY_MASK	(~0x3UL)

static inline bool futex_in_32bit_syscall(void)
{
#ifdef CONFIG_X86
	return !IS_ENABLED(CONFIG_64BIT) || in_32bit_syscall();
#else
	return !IS_ENABLED(CONFIG_64BIT);
#endif
}

static inline void futex_init_task(struct task_struct *tsk)
{
	tsk->futex_robust_lists = NULL;
	INIT_LIST_HEAD(&tsk->pi_state_list);
	tsk->pi_state_cache = NULL;
	tsk->futex_state = FUTEX_STATE_OK;
	mutex_init(&tsk->futex_exit_mutex);
}

void futex_exit_recursive(struct task_struct *tsk);
void futex_exit_release(struct task_struct *tsk);
void futex_exec_release(struct task_struct *tsk);

long do_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
	      u32 __user *uaddr2, u32 val2, u32 val3);
int futex_hash_prctl(unsigned long arg2, unsigned long arg3, unsigned long arg4);

int futex_robust_list_set(uintptr_t head, enum robust_list2_cmd cmd, unsigned int index);

#ifdef CONFIG_FUTEX_PRIVATE_HASH
int futex_hash_allocate_default(void);
void futex_hash_free(struct mm_struct *mm);
int futex_mm_init(struct mm_struct *mm);

#else /* !CONFIG_FUTEX_PRIVATE_HASH */
static inline int futex_hash_allocate_default(void) { return 0; }
static inline int futex_hash_free(struct mm_struct *mm) { return 0; }
static inline int futex_mm_init(struct mm_struct *mm) { return 0; }
#endif /* CONFIG_FUTEX_PRIVATE_HASH */

#else /* !CONFIG_FUTEX */
static inline void futex_init_task(struct task_struct *tsk) { }
static inline void futex_exit_recursive(struct task_struct *tsk) { }
static inline void futex_exit_release(struct task_struct *tsk) { }
static inline void futex_exec_release(struct task_struct *tsk) { }
static inline long do_futex(u32 __user *uaddr, int op, u32 val,
			    ktime_t *timeout, u32 __user *uaddr2,
			    u32 val2, u32 val3)
{
	return -EINVAL;
}
static inline int futex_hash_prctl(unsigned long arg2, unsigned long arg3, unsigned long arg4)
{
	return -EINVAL;
}
static inline int futex_hash_allocate_default(void)
{
	return 0;
}
static inline int futex_hash_free(struct mm_struct *mm) { return 0; }
static inline int futex_mm_init(struct mm_struct *mm) { return 0; }

#endif

#endif
