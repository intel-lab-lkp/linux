// SPDX-License-Identifier: GPL-2.0
#include <linux/smp.h>
#include <linux/timex.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/cpufreq.h>
#include <linux/proc_fs.h>
#include <asm/usercfi.h>

#ifdef CONFIG_RISCV_USER_CFI

void arch_proc_pid_thread_features(struct seq_file *m, struct task_struct *task)
{
	seq_puts(m, "riscv_thread_features:\t");
	if (is_shstk_enabled(task))
		seq_puts(m, "shstk_enabled ");

	if (is_indir_lp_enabled(task))
		seq_puts(m, "lpad_enabled ");

	seq_putc(m, '\n');

	seq_puts(m, "riscv_thread_features_locked:\t");
	is_shstk_locked(task) ? seq_puts(m, "shstk_locked ") : seq_puts(m, "shstk_unlocked ");
	is_indir_lp_locked(task) ? seq_puts(m, "lpad_locked ") : seq_puts(m, "lpad_unlocked ");
	seq_putc(m, '\n');
}
#endif /* CONFIG_RISCV_USER_CFI */
