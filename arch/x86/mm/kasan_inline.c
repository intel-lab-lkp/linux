// SPDX-License-Identifier: GPL-2.0
#include <linux/kasan.h>
#include <linux/kdebug.h>

void kasan_inline_handler(struct pt_regs *regs)
{
	int metadata = regs->cx;
	u64 addr = regs->di;
	u64 pc = regs->ip;
	bool recover = metadata & KASAN_ECX_RECOVER;
	bool write = metadata & KASAN_ECX_WRITE;
	size_t size = KASAN_ECX_SIZE(metadata);

	if (user_mode(regs))
		return;

	if (!kasan_report((void *)addr, size, write, pc))
		return;

	kasan_die_unless_recover(recover, "Oops - KASAN", regs, metadata, die);
}
