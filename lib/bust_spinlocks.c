// SPDX-License-Identifier: GPL-2.0
/*
 * lib/bust_spinlocks.c
 *
 * Provides a minimal bust_spinlocks for architectures which don't
 * have one of their own.
 *
 * bust_spinlocks() clears any spinlocks which would prevent oops, die(), BUG()
 * and panic() information from reaching the user.
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/panic.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include <linux/wait.h>
#include <linux/vt_kern.h>
#include <linux/console.h>

void bust_spinlocks(int yes)
{
	int old = READ_ONCE(oops_in_progress);

	if (yes) {
		if (in_task())
			current->in_oops++;
		do {
		} while (!try_cmpxchg(&oops_in_progress, &old, old + 1));
		if (!old && !panic_in_progress())
			printk_mark_panic_start();
	} else {
		if (in_task() && current->in_oops)
			current->in_oops--;
		console_unblank();
		old = READ_ONCE(oops_in_progress);
		do {
			if (unlikely(old <= 0))
				return;
		} while (!try_cmpxchg(&oops_in_progress, &old, old - 1));
		if (old == 1)
			wake_up_klogd();
	}
}
