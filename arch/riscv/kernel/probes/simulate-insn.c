// SPDX-License-Identifier: GPL-2.0+

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>

#include <asm/insn.h>
#include "simulate-insn.h"

bool __kprobes simulate_jal(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	s32 imm = riscv_insn_jal_extract_imm(opcode);
	u32 index = riscv_insn_jal_extract_xd(opcode);

	riscv_insn_reg_set_val((unsigned long *)regs, index, addr + 4);

	instruction_pointer_set(regs, addr + imm);

	return true;
}

bool __kprobes simulate_jalr(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long base_addr;
	s32 imm = riscv_insn_jalr_extract_imm(opcode);
	u32 rd_index = riscv_insn_jalr_extract_xd(opcode);
	u32 rs1_index = riscv_insn_jalr_extract_xs1(opcode);

	base_addr = riscv_insn_reg_get_val((unsigned long *)regs, rs1_index);

	riscv_insn_reg_set_val((unsigned long *)regs, rd_index, addr  + 4);

	instruction_pointer_set(regs, (base_addr + imm) & ~1);

	return true;
}

bool __kprobes simulate_auipc(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	u32 rd_index = riscv_insn_auipc_extract_xd(opcode);
	unsigned long rd_val = addr + (s32)riscv_insn_auipc_extract_imm(opcode);

	riscv_insn_reg_set_val((unsigned long *)regs, rd_index, rd_val);

	instruction_pointer_set(regs, addr + 4);

	return true;
}

bool __kprobes simulate_beq(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(beq, (unsigned long *)regs, opcode, addr, ==, unsigned long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_bne(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(bne, (unsigned long *)regs, opcode, addr, !=, unsigned long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_blt(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(blt, (unsigned long *)regs, opcode, addr, <, long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_bge(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(bge, (unsigned long *)regs, opcode, addr, >=, long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_bltu(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(bltu, (unsigned long *)regs, opcode, addr, <, unsigned long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_bgeu(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;

	next_addr = riscv_insn_branch(bgeu, (unsigned long *)regs, opcode, addr, >=, unsigned long);
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_c_j(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	s32 offset = riscv_insn_c_j_extract_imm(opcode);

	instruction_pointer_set(regs, addr + offset);

	return true;
}

bool __kprobes simulate_c_jr(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;
	unsigned long *regs_ptr = (unsigned long *)regs;

	/* xs1 == 0 is invalid so riscv_insn_reg_get_val() isn't needed */
	next_addr = regs_ptr[riscv_insn_c_jr_extract_xs1(opcode)];
	instruction_pointer_set(regs, next_addr);

	return true;
}

bool __kprobes simulate_c_jalr(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	unsigned long next_addr;
	unsigned long *regs_ptr = (unsigned long *)regs;

	/* xs1 == 0 is invalid so riscv_insn_reg_get_val() isn't needed */
	next_addr = regs_ptr[riscv_insn_c_jalr_extract_xs1(opcode)];
	instruction_pointer_set(regs, next_addr);

	regs->ra = addr + 2;
	return true;
}

bool __kprobes simulate_c_bnez(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	u32 rs1;
	unsigned long offset;
	unsigned long *regs_ptr = (unsigned long *)regs;

	rs1 = riscv_insn_c_bnez_extract_xs1(opcode);
	if (regs_ptr[8 + rs1] != 0)
		offset = riscv_insn_c_bnez_extract_imm(opcode);
	else
		offset = 2;

	instruction_pointer_set(regs, addr + offset);

	return true;
}

bool __kprobes simulate_c_beqz(u32 opcode, unsigned long addr, struct pt_regs *regs)
{
	u32 rs1;
	unsigned long offset;
	unsigned long *regs_ptr = (unsigned long *)regs;

	rs1 = riscv_insn_c_beqz_extract_xs1(opcode);
	if (regs_ptr[8 + rs1] == 0)
		offset = riscv_insn_c_beqz_extract_imm(opcode);
	else
		offset = 2;

	instruction_pointer_set(regs, addr + offset);

	return true;
}
