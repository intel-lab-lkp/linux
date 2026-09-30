// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenRISC instruction utils
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * OpenRISC implementation:
 * Copyright (C) 2026 Sahil Siddiq <sahilcdq0@gmail.com>
 */

#include <linux/ptrace.h>
#include <asm/insn-def.h>
#include <asm/cpuinfo.h>

DEFINE_STATIC_KEY_TRUE(delay_slot);

void simulate_pc(struct pt_regs *regs, unsigned long pc_insn)
{
	int page_offset;
	unsigned int rd, op;

	page_offset = sign_extend32(
		(pc_insn & OPENRISC_ADRP_IMM_MASK) << OPENRISC_ADRP_IMM_SHIFT, 31);
	rd = (pc_insn & OPENRISC_ADRP_REG_MASK) >> OPENRISC_ADRP_REG_SHIFT;
	op = get_6_bit_opcode(pc_insn);

	switch (op) {
	case OPENRISC_OPCODE_ADRP:
		regs->gpr[rd] = page_offset + (regs->pc & OPENRISC_ADRP_OFFSET_MASK);
		regs->pc += OPENRISC_INSN_SIZE;
		return;
	default:
		break;
	}
}

void simulate_branch(struct pt_regs *regs, unsigned long jmp_insn)
{
	int displacement;
	unsigned int rb, op, link_offset, temp;

	displacement = sign_extend32(
		((jmp_insn) & OPENRISC_JUMP_IMM_MASK) << OPENRISC_JUMP_IMM_SHIFT, 27);
	rb = (jmp_insn & OPENRISC_JUMP_REG_MASK) >> OPENRISC_JUMP_REG_SHIFT;
	op = get_6_bit_opcode(jmp_insn);
	link_offset = OPENRISC_INSN_SIZE;

	if (static_branch_likely(&delay_slot))
		link_offset <<= 1;

	switch (op) {
	case OPENRISC_OPCODE_J: /* l.j */
		regs->pc += displacement;
		return;
	case OPENRISC_OPCODE_JAL: /* l.jal */
		regs->gpr[9] = regs->pc + link_offset;
		regs->pc += displacement;
		return;
	case OPENRISC_OPCODE_BNF: /* l.bnf */
		if (regs->sr & SPR_SR_F)
			regs->pc += link_offset;
		else
			regs->pc += displacement;
		return;
	case OPENRISC_OPCODE_BF: /* l.bf */
		if (regs->sr & SPR_SR_F)
			regs->pc += displacement;
		else
			regs->pc += link_offset;
		return;
	case OPENRISC_OPCODE_JR: /* l.jr */
		regs->pc = regs->gpr[rb];
		return;
	case OPENRISC_OPCODE_JALR: /* l.jalr */
		temp = regs->pc + link_offset;
		regs->pc = regs->gpr[rb];
		regs->gpr[9] = temp;
		return;
	default:
		break;
	}
}
