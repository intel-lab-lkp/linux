// SPDX-License-Identifier: GPL-2.0
#include <linux/kasan.h>
#include <linux/kdebug.h>

bool kasan_inline_handler(struct pt_regs *regs)
{
	int metadata = regs->ax;
	u64 addr = regs->di;
	u64 pc = regs->ip;
	bool recover = metadata & KASAN_RAX_RECOVER;
	bool write = metadata & KASAN_RAX_WRITE;
	size_t size = KASAN_RAX_SIZE(metadata);

	if (user_mode(regs))
		return false;

	if (!kasan_report((void *)addr, size, write, pc))
		return false;

	if (kasan_multi_shot_enabled())
		return true;

	kasan_inline_recover(recover, "Oops - KASAN", regs, metadata, die);

	return true;
}
