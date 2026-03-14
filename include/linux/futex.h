/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/mm_types.h>

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

static inline void futex_init_task(struct task_struct *tsk)
{
	memset(&tsk->futex, 0, sizeof(tsk->futex));
	INIT_LIST_HEAD(&tsk->futex.pi_state_list);
	tsk->futex.state = FUTEX_STATE_OK;
	mutex_init(&tsk->futex.exit_mutex);
}

void futex_exit_recursive(struct task_struct *tsk);
void futex_exit_release(struct task_struct *tsk);
void futex_exec_release(struct task_struct *tsk);

long do_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
	      u32 __user *uaddr2, u32 val2, u32 val3);
int futex_hash_prctl(unsigned long arg2, unsigned long arg3, unsigned long arg4);

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
#endif /* !CONFIG_FUTEX */

#ifdef CONFIG_FUTEX_ROBUST_UNLOCK
#include <asm/futex_robust.h>

void __futex_fixup_robust_unlock(struct pt_regs *regs, struct futex_unlock_cs_range *csr);

static inline bool futex_within_robust_unlock(struct pt_regs *regs,
					      struct futex_unlock_cs_range *csr)
{
	unsigned long ip = instruction_pointer(regs);

	return ip >= csr->start_ip && ip < csr->end_ip;
}

static inline void futex_fixup_robust_unlock(struct pt_regs *regs)
{
	struct futex_unlock_cs_range *csr;

	/*
	 * Avoid dereferencing current->mm if not returning from interrupt.
	 * current->rseq.event is going to be used anyway in the exit to user
	 * code, so bringing it in is not a big deal.
	 */
	if (!current->rseq.event.user_irq)
		return;

	csr = current->mm->futex.unlock_cs_ranges;
	if (unlikely(futex_within_robust_unlock(regs, csr))) {
		__futex_fixup_robust_unlock(regs, csr);
		return;
	}

	/* Multi sized robust lists are only supported with CONFIG_COMPAT */
	if (IS_ENABLED(CONFIG_COMPAT) && current->mm->futex.unlock_cs_num_ranges == 2) {
		if (unlikely(futex_within_robust_unlock(regs, ++csr)))
			__futex_fixup_robust_unlock(regs, csr);
	}
}
#else /* CONFIG_FUTEX_ROBUST_UNLOCK */
static inline void futex_fixup_robust_unlock(struct pt_regs *regs) {}
#endif /* !CONFIG_FUTEX_ROBUST_UNLOCK */

#endif
