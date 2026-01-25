// SPDX-License-Identifier: GPL-2.0-only
#include <linux/linkage.h>
#include <linux/rseq_entry.h>
#include <linux/irq-entry-common.h>
#include <linux/kstack_erase.h>

/*
 * arm_exit_to_user_mode_no_work_pending - Fixup state when exiting to user mode
 *
 * The calling code satisfies (arch_irqs_disabled() && !lockdep_hardirqs_enabled()).
 * When the function returns, the state satisfies (arch_irqs_disabled() &&
 * lockdep_hardirqs_enabled()).
 */
asmlinkage __section(".entry.text")
void arm_exit_to_user_mode_no_work_pending(void)
{
	rseq_irqentry_exit_to_user_mode();
	exit_to_user_mode();
#ifdef CONFIG_KSTACK_ERASE
	stackleak_erase_on_task_stack();
#endif
}
