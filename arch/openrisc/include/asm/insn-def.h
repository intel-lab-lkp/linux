/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Chen Miao
 */

#include <asm/spr.h>
#include <asm/spr_defs.h>
#include <linux/bits.h>

#ifndef __ASM_OPENRISC_INSN_DEF_H
#define __ASM_OPENRISC_INSN_DEF_H

/* or1k nop instruction code */
#define OPENRISC_INSN_CSYNC	0x23000000U
#define OPENRISC_INSN_MACRC	0x18010000U
#define OPENRISC_MASK_MACRC	0xfc01ffffU
#define OPENRISC_INSN_MSYNC	0x22000000U
#define OPENRISC_INSN_NOP	0x15000000U
#define OPENRISC_INSN_PSYNC	0x22800000U
#define OPENRISC_INSN_SYS	0x20000000U
#define OPENRISC_INSN_TRAP	0x21000000U

#define OPENRISC_OPCODE_ADRP	0x02
#define OPENRISC_OPCODE_BF		0x04
#define OPENRISC_OPCODE_BNF		0x03
#define OPENRISC_OPCODE_J		0x00
#define OPENRISC_OPCODE_JAL		0x01
#define OPENRISC_OPCODE_JALR	0x12
#define OPENRISC_OPCODE_JR		0x11
#define OPENRISC_OPCODE_LWA		0x1b
#define OPENRISC_OPCODE_MFSPR	0x2d
#define OPENRISC_OPCODE_MTSPR	0x30
#define OPENRISC_OPCODE_RFE		0x09
#define OPENRISC_OPCODE_SWA		0x33

#define OPENRISC_ADRP_IMM_MASK		GENMASK(18, 0)
#define OPENRISC_ADRP_OFFSET_MASK	GENMASK(31, 13)
#define OPENRISC_ADRP_IMM_SHIFT		13
#define OPENRISC_ADRP_REG_MASK		GENMASK(25, 0)
#define OPENRISC_ADRP_REG_SHIFT		21
#define OPENRISC_JUMP_IMM_MASK		GENMASK(25, 0)
#define OPENRISC_JUMP_IMM_SHIFT		2
#define OPENRISC_JUMP_REG_MASK		GENMASK(15, 11)
#define OPENRISC_JUMP_REG_SHIFT		11

#define OPENRISC_MASK_SYS	GENMASK(31, 16)
#define OPENRISC_MASK_TRAP	GENMASK(31, 16)
#define OPENRISC_MASK_BCODE	GENMASK(15, 0)

#define OPENRISC_OPCODE_MASK	0x3f
#define OPENRISC_OPCODE_SHIFT	0x1a

struct pt_regs;

#define OPENRISC_INSN_SIZE	4

/* Helpers for working with l.trap */
static inline unsigned long __emit_trap(unsigned long code)
{
	return (code & OPENRISC_MASK_BCODE) | OPENRISC_INSN_TRAP;
}

static __always_inline unsigned char get_6_bit_opcode(unsigned long insn)
{
	return (insn >> OPENRISC_OPCODE_SHIFT) & OPENRISC_OPCODE_MASK;
}

void simulate_pc(struct pt_regs *regs, unsigned long jmp);
void simulate_branch(struct pt_regs *regs, unsigned long jmp);

#endif /* __ASM_OPENRISC_INSN_DEF_H */
