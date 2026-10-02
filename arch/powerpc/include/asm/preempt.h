/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PPC64 stores preempt count in PACA.
 * PPC32 stores preempt count in thread_info.
 */

#ifndef __ASM_POWERPC_PREEMPT_H
#define __ASM_POWERPC_PREEMPT_H

#ifdef CONFIG_PPC64
#include <linux/thread_info.h>
#include <asm/paca.h>

#define PREEMPT_ENABLED	(0)

static __always_inline int preempt_count(void)
{
	return READ_ONCE(local_paca->preempt_count);
}

static __always_inline void preempt_count_set(int pc)
{
	WRITE_ONCE(local_paca->preempt_count, pc);
}

#define init_task_preempt_count(p) do { } while (0)
#define init_idle_preempt_count(p, cpu) do { } while (0)

static inline void set_preempt_need_resched(void)
{
}

static inline void clear_preempt_need_resched(void)
{
}

static inline bool test_preempt_need_resched(void)
{
	return false;
}

static __always_inline void __preempt_count_add(int val)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	pc += val;
	WRITE_ONCE(local_paca->preempt_count, pc);
}

static __always_inline void __preempt_count_sub(int val)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	pc -= val;
	WRITE_ONCE(local_paca->preempt_count, pc);
}

static inline int __preempt_count_add_return(int val)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	pc += val;
	WRITE_ONCE(local_paca->preempt_count, pc);
	return pc;
}

static inline int __preempt_count_sub_return(int val)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	pc -= val;
	WRITE_ONCE(local_paca->preempt_count, pc);
	return pc;
}

static inline bool __preempt_count_dec_and_test(void)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	WRITE_ONCE(local_paca->preempt_count, --pc);
	return !pc && tif_need_resched();
}

static inline bool should_resched(int preempt_offset)
{
	int pc = READ_ONCE(local_paca->preempt_count);

	return unlikely(pc == preempt_offset && tif_need_resched());
}

#ifdef CONFIG_PREEMPTION
void preempt_schedule(void);
void preempt_schedule_notrace(void);

#define __preempt_schedule()		preempt_schedule()
#define __preempt_schedule_notrace()	preempt_schedule_notrace()
#endif /* CONFIG_PREEMPTION */

#else /* !CONFIG_PPC64 */
#include <asm-generic/preempt.h>
#endif

#endif
