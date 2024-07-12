/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_ASI_H
#define _ASM_X86_ASI_H

#include <asm-generic/asi.h>

#include <asm/pgtable_types.h>
#include <asm/percpu.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <linux/sched.h>

#ifdef CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION

/*
 * Overview of API usage by ASI clients:
 *
 * Setup: First call asi_init() to create a domain. At present only one domain
 * can be created per mm per class, but it's safe to asi_init() this domain
 * multiple times. For each asi_init() call you must call asi_destroy() AFTER
 * you are certain all CPUs have exicted the restricted address space (by
 * calling asi_exit()).
 *
 * Runtime usage:
 *
 * 1. Call asi_enter() to switch to the restricted address space. This can't be
 *    from an interrupt or exception handler and preemption must be disabled.
 *
 * 2. Execute untrusted code.
 *
 * 3. Call asi_relax() to inform the ASI subsystem that untrusted code execution
 *    is finished. This doesn't cause any address space change.
 *
 * 4. Either:
 *
 *    a. Go back to 1.
 *
 *    b. Call asi_exit() before returning to userspace. This immediately
 *       switches to the unrestricted address space.
 *
 * The region between 1 and 3 is called the "ASI critical section". During the
 * critical section, it is a bug to access any sensitive data, and you mustn't
 * sleep.
 *
 * The restriction on sleeping is not really a fundamental property of ASI.
 * However for performance reasons it's important that the critical section is
 * absolutely as short as possible. So the ability to do sleepy things like
 * taking mutexes oughtn't to confer any convenience on API users.
 *
 * Similarly to the issue of sleeping, the need to asi_exit in case 4b is not a
 * fundamental property of the system but a limitation of the current
 * implementation. With further work it is possible to context switch
 * from and/or to the restricted address space, and to return to userspace
 * directly from the restricted address space, or _in_ it.
 *
 * Note that the critical section only refers to the direct execution path from
 * asi_enter to asi_relax: it's fine to access sensitive data from exceptions
 * and interrupt handlers that occur during that time. ASI will re-enter the
 * restricted address space before returning from the outermost
 * exception/interrupt.
 *
 * Note: ASI does not modify KPTI behaviour; when ASI and KPTI run together
 * there are 2+N address spaces per task: the unrestricted kernel address space,
 * the user address space, and one restricted (kernel) address space for each of
 * the N ASI classes.
 */

/* Try to avoid this outside of hot code (see comment on _static_cpu_has). */
#define static_asi_enabled() cpu_feature_enabled(X86_FEATURE_ASI)

#define ASI_MAX_NUM_ORDER	2
/*
 * We include an ASI identifier in the higher bits of PCID to use
 * different PCID for restricted ASIs from non-restricted ASIs (see asi_pcid).
 * The ASI identifier we use for this is asi_index + 1, as asi_index
 * starts from 0. The -1 below for ASI_MAX_NUM comes from this PCID
 * space availability.
 */
#define ASI_MAX_NUM		((1 << ASI_MAX_NUM_ORDER) - 1)

struct asi_hooks {
	/*
	 * Both of these functions MUST be idempotent and re-entrant. They will
	 * be called in no particular order and with no particular symmetry wrt.
	 * the number of calls. They are part of the ASI critical section, so
	 * they must not sleep and must not access sensitive data.
	 */
	void (*post_asi_enter)(void);
	void (*pre_asi_exit)(void);
};

/*
 * An ASI class is a type of isolation that can be applied to a process. A
 * process may have a domain for each class.
 */
struct asi_class {
	struct asi_hooks ops;
	const char *name;
};

/*
 * An ASI domain (struct asi) represents a restricted address space. The
 * unrestricted address space (and user address space under PTI) are not
 * represented as a domain.
 */
struct asi {
	pgd_t *pgd;
	struct asi_class *class;
	struct mm_struct *mm;
	int64_t ref_count;
	u16 index;
};

DECLARE_PER_CPU_ALIGNED(struct asi *, curr_asi);

void asi_check_boottime_disable(void);

void asi_init_mm_state(struct mm_struct *mm);

int  asi_register_class(const char *name, const struct asi_hooks *ops);
void asi_unregister_class(int index);

int  asi_init(struct mm_struct *mm, int asi_index, struct asi **out_asi);
void asi_destroy(struct asi *asi);

/* Enter an ASI domain (restricted address space) and begin the critical section. */
void asi_enter(struct asi *asi);

/*
 * Leave the "tense" state if we are in it, i.e. end the critical section. We
 * will stay relaxed until the next asi_enter.
 */
void asi_relax(void);

/* Immediately exit the restricted address space if in it */
void asi_exit(void);

int  asi_map_gfp(struct asi *asi, void *addr, size_t len, gfp_t gfp_flags);
int  asi_map(struct asi *asi, void *addr, size_t len);
void asi_unmap(struct asi *asi, void *addr, size_t len);
void asi_flush_tlb_range(struct asi *asi, void *addr, size_t len);

static inline void asi_init_thread_state(struct thread_struct *thread)
{
	thread->asi_state.intr_nest_depth = 0;
}

/* The target is the domain we'll enter when returning to process context. */
static __always_inline struct asi *asi_get_target(struct task_struct *p)
{
	return static_asi_enabled()
	       ? p->thread.asi_state.target
	       : NULL;
}

static __always_inline void asi_set_target(struct task_struct *p,
					   struct asi *target)
{
	p->thread.asi_state.target = target;
}

static __always_inline struct asi *asi_get_current(void)
{
	return static_asi_enabled()
	       ? this_cpu_read(curr_asi)
	       : NULL;
}

/* Are we currently in a restricted address space? */
static __always_inline bool asi_is_restricted(void)
{
	return (bool)asi_get_current();
}

/*
 * If we exit/have exited, can we stay that way until the next asi_enter?
 *
 * When ASI is disabled, this returns true.
 */
static __always_inline bool asi_is_relaxed(void)
{
	return !asi_get_target(current);
}

/*
 * Is the current task in the critical section?
 *
 * This is just the inverse of !asi_is_relaxed(). We have both functions in
 * order to help write intuitive client code. In particular, asi_is_tense
 * returns false when ASI is disabled, which is judged to make user code more
 * obvious.
 */
static __always_inline bool asi_is_tense(void)
{
	return !asi_is_relaxed();
}

static __always_inline pgd_t *asi_pgd(struct asi *asi)
{
	return asi ? asi->pgd : NULL;
}

static __always_inline void asi_intr_enter(void)
{
	if (static_asi_enabled() && asi_is_tense()) {
		current->thread.asi_state.intr_nest_depth++;
		barrier();
	}
}

void __asi_enter(void);

static __always_inline void asi_intr_exit(void)
{
	if (static_asi_enabled() && asi_is_tense()) {
		/*
		 * If an access to sensitive memory got reordered after the
		 * decrement, the #PF handler for that access would see a value
		 * of 0 for the counter and re-__asi_enter before returning to
		 * the faulting access, triggering an infinite PF loop.
		 */
		barrier();

		if (--current->thread.asi_state.intr_nest_depth == 0) {
			/*
			 * If the decrement got reordered after __asi_enter, an
			 * interrupt that came between __asi_enter and the
			 * decrement would always see a nonzero value for the
			 * counter so it wouldn't call __asi_enter again and we
			 * would return to process context in the wrong address
			 * space.
			 */
			barrier();
			__asi_enter();
		}
	}
}

/*
 * Returns the nesting depth of interrupts/exceptions that have interrupted the
 * ongoing critical section. If the current task is not in a critical section
 * this is 0.
 */
static __always_inline int asi_intr_nest_depth(void)
{
	return current->thread.asi_state.intr_nest_depth;
}

/*
 * Remember that interrupts/exception don't count as the critical section. If
 * you want to know if the current task is in the critical section use
 * asi_is_tense().
 */
static __always_inline bool asi_in_critical_section(void)
{
	return asi_is_tense() && !asi_intr_nest_depth();
}

#define INIT_MM_ASI(init_mm) \
	.asi_init_lock = __MUTEX_INITIALIZER(init_mm.asi_init_lock),

#endif /* CONFIG_MITIGATION_ADDRESS_SPACE_ISOLATION */

#endif
