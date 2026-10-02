/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PPC64 stores preempt count in PACA.
 * PPC32 stores preempt count in thread_info.
 */

#ifndef __ASM_POWERPC_PREEMPT_H
#define __ASM_POWERPC_PREEMPT_H

#ifdef CONFIG_PPC64
#include <asm/paca.h>

#define PREEMPT_NEED_RESCHED	BIT(32)
#define PREEMPT_ENABLED	(PREEMPT_NEED_RESCHED)

static __always_inline int preempt_count(void)
{
	return READ_ONCE(local_paca->preempt.count);
}

static __always_inline void preempt_count_set(u64 pc)
{
	/* Preserve existing value of PREEMPT_NEED_RESCHED */
	WRITE_ONCE(local_paca->preempt.count, pc);
}

#define init_task_preempt_count(p) do { } while (0)
#define init_idle_preempt_count(p, cpu) do { } while (0)

static inline void set_preempt_need_resched(void)
{
	WRITE_ONCE(local_paca->preempt.need_resched, 0);
}

static inline void clear_preempt_need_resched(void)
{
	WRITE_ONCE(local_paca->preempt.need_resched, 1);
}

static inline bool test_preempt_need_resched(void)
{
	return !READ_ONCE(local_paca->preempt.need_resched);
}

static __always_inline void __preempt_count_add(int val)
{
	u32 pc = READ_ONCE(local_paca->preempt.count);

	pc += val;
	WRITE_ONCE(local_paca->preempt.count, pc);
}

static __always_inline void __preempt_count_sub(int val)
{
	u32 pc = READ_ONCE(local_paca->preempt.count);

	pc -= val;
	WRITE_ONCE(local_paca->preempt.count, pc);
}

static inline int __preempt_count_add_return(int val)
{
	u32 pc = READ_ONCE(local_paca->preempt.count);

	pc += val;
	WRITE_ONCE(local_paca->preempt.count, pc);
	return pc;
}

static inline int __preempt_count_sub_return(int val)
{
	u32 pc = READ_ONCE(local_paca->preempt.count);

	pc -= val;
	WRITE_ONCE(local_paca->preempt.count, pc);
	return pc;
}

static inline bool __preempt_count_dec_and_test(void)
{
	u64 pc = READ_ONCE(local_paca->preempt_count);

	/* Update only the count field, leaving need_resched unchanged */
	WRITE_ONCE(local_paca->preempt.count, --pc);
	/*
	 * If we wrote back all zeroes, then we're preemptible and in
	 * need of a reschedule. Otherwise, we need to reload the
	 * preempt_count in case the need_resched flag was cleared by an
	 * interrupt occurring between the non-atomic READ_ONCE/WRITE_ONCE
	 * pair.
	 */
	return !pc || !READ_ONCE(local_paca->preempt_count);
}

static inline bool should_resched(int preempt_offset)
{
	u64 pc = READ_ONCE(local_paca->preempt_count);
	return unlikely(pc == preempt_offset);
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
