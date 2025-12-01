// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c), 2025 Alibaba Damo Academy
 * Authors:
 *	Guo Ren <guoren@kernel.org>
 */

#include <linux/static_call.h>
#include <asm/qspinlock_paravirt.h>
#include <asm/sbi.h>

#define CREATE_TRACE_POINTS
#include "trace_events_filter_paravirt.h"

void pv_kick(int cpu)
{
	trace_pv_kick(smp_processor_id(), cpu);

	sbi_ecall(SBI_EXT_PVLOCK, SBI_EXT_PVLOCK_KICK_CPU,
		  cpuid_to_hartid_map(cpu), 0, 0, 0, 0, 0);
	return;
}

void pv_wait(u8 *ptr, u8 val)
{
	unsigned long flags;

	if (in_nmi())
		return;

	local_irq_save(flags);
	if (READ_ONCE(*ptr) != val)
		goto out;

	wait_for_interrupt();

	trace_pv_wait(smp_processor_id());
out:
	local_irq_restore(flags);
}

static void native_queued_spin_unlock(struct qspinlock *lock)
{
	smp_store_release(&lock->locked, 0);
}

DEFINE_STATIC_CALL(pv_queued_spin_lock_slowpath, native_queued_spin_lock_slowpath);
EXPORT_STATIC_CALL(pv_queued_spin_lock_slowpath);

DEFINE_STATIC_CALL(pv_queued_spin_unlock, native_queued_spin_unlock);
EXPORT_STATIC_CALL(pv_queued_spin_unlock);

DEFINE_STATIC_KEY_FALSE(virt_spin_lock_key);

bool __init pv_qspinlock_init(void)
{
	if (num_possible_cpus() == 1)
		return false;

	if (!sbi_probe_extension(SBI_EXT_PVLOCK))
		return false;

	if (nopvspin) {
		static_branch_enable(&virt_spin_lock_key);
		pr_info("virt_spin_lock enabled by nopvspin\n");
		return true;
	}

	pr_info("PV qspinlocks enabled\n");
	__pv_init_lock_hash();

	static_call_update(pv_queued_spin_lock_slowpath, __pv_queued_spin_lock_slowpath);
	static_call_update(pv_queued_spin_unlock, __pv_queued_spin_unlock);

	return true;
}

bool pv_is_native_spin_unlock(void)
{
	if (static_call_query(pv_queued_spin_unlock) == native_queued_spin_unlock)
		return true;
	else
		return false;
}
