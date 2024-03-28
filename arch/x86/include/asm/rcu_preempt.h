/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_RCU_PREEMPT_H
#define __ASM_RCU_PREEMPT_H

#include <asm/rmwcc.h>
#include <asm/percpu.h>
#include <asm/current.h>

#ifdef CONFIG_PCPU_RCU_PREEMPT_COUNT

/* We use the MSB mostly because its available */
#define RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED	0x80000000

/*
 * We use the RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED bit as an inverted
 * current->rcu_read_unlock_special.s such that a decrement hitting 0
 * means we can and should call rcu_read_unlock_special().
 */
#define RCU_PREEMPT_INIT	(0 + RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED)

/*
 * We mask the RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED bit so as not to
 * confuse all current users that think a non-zero value indicates we
 * are in a critical section.
 */
static inline int pcpu_rcu_preempt_count(void)
{
	return raw_cpu_read_4(pcpu_hot.rcu_preempt_count) & ~RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED;
}

static inline void pcpu_rcu_preempt_count_set(int count)
{
	int old, new;

	old = raw_cpu_read_4(pcpu_hot.rcu_preempt_count);
	do {
		new = (old & RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED) |
			(count & ~RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED);
	} while (!raw_cpu_try_cmpxchg_4(pcpu_hot.rcu_preempt_count, &old, new));
}

/*
 * We fold the RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED bit into the RCU
 * preempt count such that rcu_read_unlock() can decrement and test for
 * the need of unlock-special handling with a single instruction.
 *
 * We invert the actual bit, so that when the decrement hits 0 we know
 * we both reach a quiescent state (no rcu preempt count) and need to
 * handle unlock-special (the bit is cleared), normally to report the
 * quiescent state immediately.
 */

static inline void pcpu_rcu_preempt_special_set(void)
{
	raw_cpu_and_4(pcpu_hot.rcu_preempt_count, ~RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED);
}

static inline void pcpu_rcu_preempt_special_clear(void)
{
	raw_cpu_or_4(pcpu_hot.rcu_preempt_count, RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED);
}

static inline bool pcpu_rcu_preempt_special_test(void)
{
	return !(raw_cpu_read_4(pcpu_hot.rcu_preempt_count) & RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED);
}

static inline void pcpu_rcu_preempt_switch(int count, bool special)
{
	if (likely(!special))
		raw_cpu_write(pcpu_hot.rcu_preempt_count, count | RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED);
	else
		raw_cpu_write(pcpu_hot.rcu_preempt_count, count);
}

/*
 * The various rcu_preempt_count add/sub methods
 */

static __always_inline void pcpu_rcu_preempt_count_add(int val)
{
	raw_cpu_add_4(pcpu_hot.rcu_preempt_count, val);
}

static __always_inline void pcpu_rcu_preempt_count_sub(int val)
{
	raw_cpu_add_4(pcpu_hot.rcu_preempt_count, -val);
}

/*
 * Because we keep RCU_PREEMPT_UNLOCK_SPECIAL_INVERTED set when we do
 * _not_ need to handle unlock-special for a fast-path decrement.
 */
static __always_inline bool pcpu_rcu_preempt_count_dec_and_test(void)
{
	return GEN_UNARY_RMWcc("decl", __my_cpu_var(pcpu_hot.rcu_preempt_count), e,
			       __percpu_arg([var]));
}

#define pcpu_rcu_read_unlock_special()						\
do {										\
	rcu_read_unlock_special();						\
} while (0)

#endif // #ifdef CONFIG_PCPU_RCU_PREEMPT_COUNT

#endif /* __ASM_RCU_PREEMPT_H */
