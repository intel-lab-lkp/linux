/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TASK_STACK_H
#define _LINUX_SCHED_TASK_STACK_H

/*
 * task->stack (kernel stack) handling interfaces:
 */

#include <linux/sched.h>
#include <linux/magic.h>
#include <linux/refcount.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_THREAD_INFO_IN_TASK

/*
 * When accessing the stack of a non-current task that might exit, use
 * try_get_task_stack() instead.  task_stack_page will return a pointer
 * that could get freed out from under you.
 */
static __always_inline void *task_stack_page(const struct task_struct *task)
{
	return task->stack;
}

#define setup_thread_stack(new,old)	do { } while(0)

static __always_inline unsigned long *end_of_stack(const struct task_struct *task)
{
#ifdef CONFIG_STACK_GROWSUP
	return (unsigned long *)((unsigned long)task->stack + THREAD_SIZE) - 1;
#else
	return task->stack;
#endif
}

#elif !defined(__HAVE_THREAD_FUNCTIONS)

#define task_stack_page(task)	((void *)(task)->stack)

static inline void setup_thread_stack(struct task_struct *p, struct task_struct *org)
{
	*task_thread_info(p) = *task_thread_info(org);
	task_thread_info(p)->task = p;
}

/*
 * Return the address of the last usable long on the stack.
 *
 * When the stack grows down, this is just above the thread
 * info struct. Going any lower will corrupt the threadinfo.
 *
 * When the stack grows up, this is the highest address.
 * Beyond that position, we corrupt data on the next page.
 */
static inline unsigned long *end_of_stack(struct task_struct *p)
{
#ifdef CONFIG_STACK_GROWSUP
	return (unsigned long *)((unsigned long)task_thread_info(p) + THREAD_SIZE) - 1;
#else
	return (unsigned long *)(task_thread_info(p) + 1);
#endif
}

#endif

#ifdef CONFIG_THREAD_INFO_IN_TASK
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return refcount_inc_not_zero(&tsk->stack_refcount) ?
		task_stack_page(tsk) : NULL;
}

extern void put_task_stack(struct task_struct *tsk);
#else
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return task_stack_page(tsk);
}

static inline void put_task_stack(struct task_struct *tsk) {}
#endif

void exit_task_stack_account(struct task_struct *tsk);

#ifdef CONFIG_DYNAMIC_STACK

#define task_stack_end_corrupted(task)	0

#ifndef THREAD_PREALLOC_PAGES
#define THREAD_PREALLOC_PAGES		1
#endif

#define THREAD_DYNAMIC_PAGES						\
	((THREAD_SIZE >> PAGE_SHIFT) - THREAD_PREALLOC_PAGES)

void dynamic_stack_refill_pages(void);
bool dynamic_stack_fault(struct task_struct *tsk, unsigned long address);

/*
 * Refill and charge for the used pages.
 */
static inline void dynamic_stack(struct task_struct *tsk)
{
	if (unlikely(tsk->flags & PF_DYNAMIC_STACK)) {
		dynamic_stack_refill_pages();
		tsk->flags &= ~PF_DYNAMIC_STACK;
	}
}

static inline void set_task_stack_end_magic(struct task_struct *tsk) {}

#ifdef CONFIG_DEBUG_STACK_USAGE
static inline unsigned long stack_not_used(struct task_struct *p)
{
	struct vm_struct *vm_area = p->stack_vm_area;
	unsigned long alloc_size = vm_area->nr_pages << PAGE_SHIFT;
	unsigned long stack = (unsigned long)p->stack;
	unsigned long *n = (unsigned long *)(stack + THREAD_SIZE - alloc_size);

	while (!*n)
		n++;

	return (unsigned long)n - stack;
}
#endif /* CONFIG_DEBUG_STACK_USAGE */

#else /* !CONFIG_DYNAMIC_STACK */

#define task_stack_end_corrupted(task) \
		(*(end_of_stack(task)) != STACK_END_MAGIC)

void set_task_stack_end_magic(struct task_struct *tsk);
static inline void dynamic_stack(struct task_struct *tsk) {}

static inline bool dynamic_stack_fault(struct task_struct *tsk,
				       unsigned long address)
{
	return false;
}

#ifdef CONFIG_DEBUG_STACK_USAGE
#ifdef CONFIG_STACK_GROWSUP
static inline unsigned long stack_not_used(struct task_struct *p)
{
	unsigned long *n = end_of_stack(p);

	do {	/* Skip over canary */
		n--;
	} while (!*n);

	return (unsigned long)end_of_stack(p) - (unsigned long)n;
}
#else /* !CONFIG_STACK_GROWSUP */
static inline unsigned long stack_not_used(struct task_struct *p)
{
	unsigned long *n = end_of_stack(p);

	do {	/* Skip over canary */
		n++;
	} while (!*n);

	return (unsigned long)n - (unsigned long)end_of_stack(p);
}
#endif /* CONFIG_STACK_GROWSUP */
#endif /* CONFIG_DEBUG_STACK_USAGE */

#endif /* CONFIG_DYNAMIC_STACK */

static inline int object_is_on_stack(const void *obj)
{
	void *stack = task_stack_page(current);

	return (obj >= stack) && (obj < (stack + THREAD_SIZE));
}

extern void thread_stack_cache_init(void);

static inline int kstack_end(void *addr)
{
	/* Reliable end of stack detection:
	 * Some APM bios versions misalign the stack
	 */
	return !(((unsigned long)addr+sizeof(void*)-1) & (THREAD_SIZE-sizeof(void*)));
}

#endif /* _LINUX_SCHED_TASK_STACK_H */
