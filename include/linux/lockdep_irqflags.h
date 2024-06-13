/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_LOCKDEP_IRQFLAGS_H
#define __LINUX_LOCKDEP_IRQFLAGS_H

#include <linux/debug_locks.h>
#include <linux/kconfig.h>
#include <linux/preempt.h>
#include <asm/bug.h>
#include <asm/percpu.h>

#ifdef CONFIG_PROVE_LOCKING
DECLARE_PER_CPU(int, hardirqs_enabled);
DECLARE_PER_CPU(int, hardirq_context);
DECLARE_PER_CPU(unsigned int, lockdep_recursion);

#define __lockdep_enabled	(debug_locks && !this_cpu_read(lockdep_recursion))

#define lockdep_assert_irqs_enabled()					\
do {									\
	WARN_ON_ONCE(__lockdep_enabled && !this_cpu_read(hardirqs_enabled)); \
} while (0)

#define lockdep_assert_irqs_disabled()					\
do {									\
	WARN_ON_ONCE(__lockdep_enabled && this_cpu_read(hardirqs_enabled)); \
} while (0)

#define lockdep_assert_in_irq()						\
do {									\
	WARN_ON_ONCE(__lockdep_enabled && !this_cpu_read(hardirq_context)); \
} while (0)

#define lockdep_assert_no_hardirq()					\
do {									\
	WARN_ON_ONCE(__lockdep_enabled && (this_cpu_read(hardirq_context) || \
					   !this_cpu_read(hardirqs_enabled))); \
} while (0)

#define lockdep_assert_preemption_enabled()				\
do {									\
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PREEMPT_COUNT)	&&		\
		     __lockdep_enabled			&&		\
		     (preempt_count() != 0		||		\
		      !this_cpu_read(hardirqs_enabled)));		\
} while (0)

#define lockdep_assert_preemption_disabled()				\
do {									\
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PREEMPT_COUNT)	&&		\
		     __lockdep_enabled			&&		\
		     (preempt_count() == 0		&&		\
		      this_cpu_read(hardirqs_enabled)));		\
} while (0)

/*
 * Acceptable for protecting per-CPU resources accessed from BH.
 * Much like in_softirq() - semantics are ambiguous, use carefully.
 */
#define lockdep_assert_in_softirq()					\
do {									\
	WARN_ON_ONCE(__lockdep_enabled			&&		\
		     (!in_softirq() || in_irq() || in_nmi()));		\
} while (0)

#else
# define lockdep_assert_irqs_enabled() do { } while (0)
# define lockdep_assert_irqs_disabled() do { } while (0)
# define lockdep_assert_in_irq() do { } while (0)
# define lockdep_assert_no_hardirq() do { } while (0)

# define lockdep_assert_preemption_enabled() do { } while (0)
# define lockdep_assert_preemption_disabled() do { } while (0)
# define lockdep_assert_in_softirq() do { } while (0)
#endif

#endif /* __LINUX_LOCKDEP_IRQFLAGS_H */
