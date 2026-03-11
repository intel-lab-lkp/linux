// SPDX-License-Identifier: GPL-2.0
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/futex.h>
#include <asm/current.h>
#include <asm/traps.h>
#include <asm/vdso.h>

enum vdso_extable_entry_type {
	VDSO_EXTABLE_ENTRY_FIXUP = 0,
	VDSO_EXTABLE_ENTRY_FUTEX = 1,
};

struct vdso_exception_table_entry {
	int type;	/* enum vdso_extable_entry_type */
	union {
		struct {
			int insn, fixup_insn;
		} fixup;
		struct {
			int start, end;
		} futex;
	};
};

bool fixup_vdso_exception(struct pt_regs *regs, int trapnr,
			  unsigned long error_code, unsigned long fault_addr)
{
	const struct vdso_image *image = current->mm->context.vdso_image;
	const struct vdso_exception_table_entry *extable;
	unsigned int nr_entries, i;
	unsigned long base;

	/*
	 * Do not attempt to fixup #DB or #BP.  It's impossible to identify
	 * whether or not a #DB/#BP originated from within an SGX enclave and
	 * SGX enclaves are currently the only use case for vDSO fixup.
	 */
	if (trapnr == X86_TRAP_DB || trapnr == X86_TRAP_BP)
		return false;

	if (!current->mm->context.vdso)
		return false;

	base =  (unsigned long)current->mm->context.vdso + image->extable_base;
	nr_entries = image->extable_len / (sizeof(*extable));
	extable = image->extable;

	for (i = 0; i < nr_entries; i++) {
		if (extable[i].type != VDSO_EXTABLE_ENTRY_FIXUP)
			continue;
		if (regs->ip == base + extable[i].fixup.insn) {
			regs->ip = base + extable[i].fixup.fixup_insn;
			regs->di = trapnr;
			regs->si = error_code;
			regs->dx = fault_addr;
			return true;
		}
	}

	return false;
}

void futex_vdso_exception(struct pt_regs *regs,
			  bool *_in_futex_vdso,
			  bool *_need_wakeup)
{
	const struct vdso_image *image = current->mm->context.vdso_image;
	const struct vdso_exception_table_entry *extable;
	bool in_futex_vdso = false, need_wakeup = false;
	unsigned int nr_entries, i;
	unsigned long base;

	if (!current->mm->context.vdso)
		goto end;

	base = (unsigned long)current->mm->context.vdso + image->extable_base;
	nr_entries = image->extable_len / (sizeof(*extable));
	extable = image->extable;

	for (i = 0; i < nr_entries; i++) {
		if (extable[i].type != VDSO_EXTABLE_ENTRY_FUTEX)
			continue;
		if (regs->ip >= base + extable[i].futex.start &&
		    regs->ip < base + extable[i].futex.end) {
			in_futex_vdso = true;
			if (regs->ax & FUTEX_WAITERS)
				need_wakeup = true;
		}
	}
end:
	*_in_futex_vdso = in_futex_vdso;
	*_need_wakeup = need_wakeup;
}
