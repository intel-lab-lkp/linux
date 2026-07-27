/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_PTDUMP_H
#define _ASM_RISCV_PTDUMP_H

#include <linux/ptdump.h>
#include <linux/seq_file.h>

struct addr_marker {
	unsigned long start_address;
	const char *name;
};

struct ptdump_prot_bits {
	u64 mask;
	const char *set;
	const char *clear;
};

struct ptdump_pg_level {
	const struct ptdump_prot_bits *bits;
	const char *name;
	u64 mask;
	int num;
};

struct ptdump_pg_state {
	struct ptdump_state ptdump;
	struct seq_file *seq;
	const struct addr_marker *marker;
	unsigned long start_address;
	unsigned long start_pa;
	unsigned long last_pa;
	int level;
	u64 current_prot;
	bool check_wx;
	unsigned long wx_pages;
};

void note_page(struct ptdump_state *pt_st, unsigned long addr, int level, u64 val);

#endif /* _ASM_RISCV_PTDUMP_H */
