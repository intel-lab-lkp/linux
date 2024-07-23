/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LOONGARCH_PARAVIRT_H
#define _ASM_LOONGARCH_PARAVIRT_H

#ifdef CONFIG_PARAVIRT

#include <linux/static_call_types.h>
struct static_key;
extern struct static_key paravirt_steal_enabled;
extern struct static_key paravirt_steal_rq_enabled;

u64 dummy_steal_clock(int cpu);
DECLARE_STATIC_CALL(pv_steal_clock, dummy_steal_clock);

static inline u64 paravirt_steal_clock(int cpu)
{
	return static_call(pv_steal_clock)(cpu);
}

int __init pv_ipi_init(void);
int __init pv_time_init(void);

#if defined(CONFIG_PARAVIRT_SPINLOCKS)
struct qspinlock;
struct pv_lock_ops {
	void (*queued_spin_lock_slowpath)(struct qspinlock *lock, u32 val);
	void (*queued_spin_unlock)(struct qspinlock *lock);
	void (*wait)(u8 *ptr, u8 val);
	void (*kick)(int cpu);
	bool (*vcpu_is_preempted)(int cpu);
};

extern struct pv_lock_ops pv_lock_ops;

void __init kvm_spinlock_init(void);
bool pv_is_native_spin_unlock(void);

static __always_inline void pv_queued_spin_lock_slowpath(struct qspinlock *lock,
		u32 val)
{
	pv_lock_ops.queued_spin_lock_slowpath(lock, val);
}

static __always_inline void pv_queued_spin_unlock(struct qspinlock *lock)
{
	pv_lock_ops.queued_spin_unlock(lock);
}

static __always_inline void pv_wait(u8 *ptr, u8 val)
{
	pv_lock_ops.wait(ptr, val);
}

static __always_inline void pv_kick(int cpu)
{
	pv_lock_ops.kick(cpu);
}

static __always_inline bool pv_vcpu_is_preempted(long cpu)
{
	return pv_lock_ops.vcpu_is_preempted(cpu);
}
#endif /* PARAVIRT_SPINLOCKS */
#else

static inline int pv_ipi_init(void)
{
	return 0;
}

static inline int pv_time_init(void)
{
	return 0;
}
#endif // CONFIG_PARAVIRT

#ifndef CONFIG_PARAVIRT_SPINLOCKS
static inline void kvm_spinlock_init(void)
{
}
#endif
#endif
