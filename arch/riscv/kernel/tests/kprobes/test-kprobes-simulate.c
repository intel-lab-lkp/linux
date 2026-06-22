// SPDX-License-Identifier: GPL-2.0+

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <kunit/test.h>

#include "../../probes/simulate-insn.h"

#include <asm/insn.h>
#include <asm/text-patching.h>

static void test_kprobe_simulate_riscv(struct kunit *test)
{
	unsigned int addr = 0xdeadbeef;
	unsigned int i = 0;

	do {
		struct pt_regs regs = { 0 };

		if (riscv_insn_is_jal(i)) {
			s32 offset = riscv_insn_jal_extract_imm(i);
			u32 xd_index = riscv_insn_jal_extract_xd(i);

			simulate_jal(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
					    "jal instruction (0x%x) incorrectly simulated", i);

			if (xd_index)
				KUNIT_EXPECT_EQ_MSG(
					test,
					riscv_insn_reg_get_val((unsigned long *)&regs, xd_index),
					addr + 4, "jal instruction (0x%x) incorrectly simulated",
					i);
		}
		if (riscv_insn_is_jalr(i)) {
			unsigned long reg_addr = 0xffff;
			s32 offset = riscv_insn_jalr_extract_imm(i);
			u32 rd_index = riscv_insn_jalr_extract_xd(i);
			u32 rs1_index = riscv_insn_jalr_extract_xs1(i);

			if (rs1_index)
				riscv_insn_reg_set_val((unsigned long *)&regs, rs1_index, reg_addr);
			else
				reg_addr = 0;

			simulate_jalr(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, (reg_addr + offset) & ~1,
					    "jalr instruction (0x%x) incorrectly simulated", i);

			if (rd_index)
				KUNIT_EXPECT_EQ_MSG(
					test,
					riscv_insn_reg_get_val((unsigned long *)&regs, rd_index),
					addr + 4, "jalr instruction (0x%x) incorrectly simulated",
					i);
		} else if (riscv_insn_is_auipc(i)) {
			s32 offset = riscv_insn_auipc_extract_imm(i);
			u32 rd_index = riscv_insn_auipc_extract_xd(i);

			simulate_auipc(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
					    "auipc instruction (0x%x) incorrectly simulated", i);

			if (rd_index)
				KUNIT_EXPECT_EQ_MSG(
					test,
					riscv_insn_reg_get_val((unsigned long *)&regs, rd_index),
					(unsigned long)addr + offset,
					"auipc instruction (0x%x) incorrectly simulated", i);
		} else if (riscv_insn_is_beq(i)) {
			s32 offset = riscv_insn_beq_extract_imm(i);
			u32 rs1_index = riscv_insn_beq_extract_xs1(i);
			u32 rs2_index = riscv_insn_beq_extract_xs2(i);

			simulate_beq(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) ==
			    riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "beq instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "beq instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_bne(i)) {
			s32 offset = riscv_insn_bne_extract_imm(i);
			u32 rs1_index = riscv_insn_bne_extract_xs1(i);
			u32 rs2_index = riscv_insn_bne_extract_xs2(i);

			simulate_bne(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) !=
			    riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "bne instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "bne instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_blt(i)) {
			s32 offset = riscv_insn_blt_extract_imm(i);
			u32 rs1_index = riscv_insn_blt_extract_xs1(i);
			u32 rs2_index = riscv_insn_blt_extract_xs2(i);

			simulate_blt(i, addr, &regs);

			if ((long)riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) <
			    (long)riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "blt instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "blt instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_bge(i)) {
			s32 offset = riscv_insn_bge_extract_imm(i);
			u32 rs1_index = riscv_insn_bge_extract_xs1(i);
			u32 rs2_index = riscv_insn_bge_extract_xs2(i);

			simulate_bge(i, addr, &regs);

			if ((long)riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) >=
			    (long)riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "bge instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "bge instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_bltu(i)) {
			s32 offset = riscv_insn_bltu_extract_imm(i);
			u32 rs1_index = riscv_insn_bltu_extract_xs1(i);
			u32 rs2_index = riscv_insn_bltu_extract_xs2(i);

			simulate_bltu(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) <
			    riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "bltu instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "bltu instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_bgeu(i)) {
			s32 offset = riscv_insn_bgeu_extract_imm(i);
			u32 rs1_index = riscv_insn_bgeu_extract_xs1(i);
			u32 rs2_index = riscv_insn_bgeu_extract_xs2(i);

			simulate_bgeu(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index) >=
			    riscv_insn_reg_get_val((unsigned long *)&regs, rs2_index)) {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
						    "bgeu instruction (0x%x) incorrectly simulated",
						    i);
			} else {
				KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + 4,
						    "bgeu instruction (0x%x) incorrectly simulated",
						    i);
			}
		} else if (riscv_insn_is_c_j(i)) {
			s32 offset = riscv_insn_c_j_extract_imm(i);

			simulate_c_j(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
					    "c.j instruction (0x%x) incorrectly simulated", i);
		} else if (riscv_insn_is_c_jr(i)) {
			u32 rs1_index = riscv_insn_c_jr_extract_xs1(i);

			simulate_c_jr(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc,
					    riscv_insn_reg_get_val((unsigned long *)&regs,
								   rs1_index),
					    "c.jr instruction (0x%x) incorrectly simulated", i);
		} else if (riscv_insn_is_c_jalr(i)) {
			unsigned long reg_addr = 0xffff;
			u32 rs1_index = riscv_insn_c_jalr_extract_xs1(i);

			if (rs1_index)
				riscv_insn_reg_set_val((unsigned long *)&regs, rs1_index, reg_addr);
			else
				reg_addr = 0;

			simulate_c_jalr(i, addr, &regs);

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, reg_addr,
					    "c.jalr instruction (0x%x) incorrectly simulated", i);

			KUNIT_EXPECT_EQ_MSG(test, regs.ra, addr + 2,
					    "c.jalr instruction (0x%x) incorrectly simulated", i);
		} else if (riscv_insn_is_c_bnez(i)) {
			u32 offset;
			u32 rs1_index = riscv_insn_c_bnez_extract_xs1(i);

			simulate_c_bnez(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index + 8) != 0)
				offset = riscv_insn_c_bnez_extract_imm(i);
			else
				offset = 2;

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
					    "c.bnez instruction (0x%x) incorrectly simulated", i);
		} else if (riscv_insn_is_c_beqz(i)) {
			u32 offset;
			u32 rs1_index = riscv_insn_c_beqz_extract_xs1(i);

			simulate_c_beqz(i, addr, &regs);

			if (riscv_insn_reg_get_val((unsigned long *)&regs, rs1_index + 8) == 0)
				offset = riscv_insn_c_beqz_extract_imm(i);
			else
				offset = 2;

			KUNIT_EXPECT_EQ_MSG(test, regs.epc, addr + offset,
					    "c.beqz instruction (0x%x) incorrectly simulated", i);
		}
	} while (++i > 0);
}

static struct kunit_case kprobes_simulate_testcases[] = {
	KUNIT_CASE_SLOW(test_kprobe_simulate_riscv),
	{}
};

static struct kunit_suite kprobes_simulate_test_suite = {
	.name = "kprobes_simulate_riscv",
	.test_cases = kprobes_simulate_testcases,
};

kunit_test_suites(&kprobes_simulate_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit test for riscv kprobes instruction simulatation");
