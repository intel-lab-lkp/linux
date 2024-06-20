/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from arch/x86/mm/pf_in.h:
 *   Copyright (C) 2024 Jialong Yang (jialong.yang@shingroup.cn)
 */

#ifndef __PF_H_
#define __PF_H_

enum OPCODE_FORMAT {
	D_FORMAT,
	DQ_FORMAT,
	DS_FORMAT,
};

struct opcode_t {
	unsigned int opcode;
	enum OPCODE_FORMAT form;
	const char *name;
};

struct trap_reason {
	unsigned long addr;
	unsigned long ip;
	struct opcode_t *opcode;
	int active_traces;
};

struct opcode_t *get_opcode(unsigned int *addr);
enum mm_io_opcode get_ins_type(struct opcode_t *opcode);
unsigned int get_ins_width(struct opcode_t *opcode);
unsigned long get_ins_val(struct trap_reason *reason, struct pt_regs *regs);
#endif /* __PF_H_ */
