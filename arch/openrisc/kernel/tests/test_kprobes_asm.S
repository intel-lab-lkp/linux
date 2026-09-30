/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KProbes unit tests for OpenRISC
 *
 * Unit tests borrowed liberally from similar work of RISC-V. All
 * original copyrights apply as per the original source declaration.
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include "test_kprobes.h"

SYM_FUNC_START(test_kprobes_j)
test_kprobes_j_addr1:
	l.j	2f
	l.nop
1:	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
	l.addi	r11,r13,0
	l.jr	r9
	l.nop
2:	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
test_kprobes_j_addr2:
	l.j	1b
	l.nop
SYM_FUNC_END(test_kprobes_j)

SYM_FUNC_START(test_kprobes_jal)
	l.addi	r15,r9,0
test_kprobes_jal_addr1:
	l.jal	2f
	l.nop
1:	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
	l.jr	r9
	l.nop
test_kprobes_jal_addr2:
2:	l.jal	1b
	l.nop
	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
	l.addi	r11,r13,0
	l.addi	r9,r15,0
	l.jr	r9
	l.nop
SYM_FUNC_END(test_kprobes_jal)

SYM_FUNC_START(test_kprobes_jr)
	l.movhi	r15,hi(2f)
	l.ori	r15,r15,lo(2f)
test_kprobes_jr_addr1:
	l.jr	r15
	l.nop
1:	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
	l.addi	r11,r13,0
	l.jr	r9
	l.nop
2:	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
	l.movhi	r15,hi(1b)
	l.ori	r15,r15,lo(1b)
test_kprobes_jr_addr2:
	l.jr	r15
	l.nop
SYM_FUNC_END(test_kprobes_jr)

SYM_FUNC_START(test_kprobes_jalr)
	l.addi	r15,r9,0
	l.movhi	r17,hi(2f)
	l.ori	r17,r17,lo(2f)
test_kprobes_jalr_addr1:
	l.jalr	r17
	l.nop
1:	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
	l.jr	r9
	l.nop
2:	l.movhi	r17,hi(1b)
	l.ori	r17,r17,lo(1b)
test_kprobes_jalr_addr2:
	l.jalr	r17
	l.nop
	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
	l.addi	r11,r13,0
	l.addi	r9,r15,0
	l.jr	r9
	l.nop
SYM_FUNC_END(test_kprobes_jalr)

SYM_FUNC_START(test_kprobes_branch)
	l.addi	r15,r0,1
	l.addi	r17,r0,0
	l.sfgts	r15,r17
test_kprobes_branch_addr1:
	l.bf	2f
	l.nop
1:	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
	l.addi	r11,r13,0
	l.jr	r9
	l.nop
2:	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
	l.sfeq	r15,r17
test_kprobes_branch_addr2:
	l.bnf	1b
	l.nop
SYM_FUNC_END(test_kprobes_branch)

SYM_FUNC_START(test_kprobes_adrp)
	l.addi	r13,r0,0
	l.addi	r15,r9,0
	l.addi	r17,r0,-8192
	l.jal	1f
	l.nop
1:	l.and	r19,r9,r17
test_kprobes_adrp_addr:
	l.adrp	r21,0
	l.sfeq	r21,r19
	l.bnf	2f
	l.nop
	l.movhi	r13,KPROBE_TEST_MAGIC_UPPER
	l.ori	r13,r13,KPROBE_TEST_MAGIC_LOWER
2:	l.addi	r9,r15,0
	l.jr	r9
	l.nop
SYM_FUNC_END(test_kprobes_adrp)

    .section .rodata
SYM_DATA_START(test_kprobes_addresses)
	.word test_kprobes_j_addr1
	.word test_kprobes_j_addr2
	.word test_kprobes_jal_addr1
	.word test_kprobes_jal_addr2
	.word test_kprobes_jr_addr1
	.word test_kprobes_jr_addr2
	.word test_kprobes_jalr_addr1
	.word test_kprobes_jalr_addr2
	.word test_kprobes_branch_addr1
	.word test_kprobes_branch_addr2
	.word test_kprobes_adrp_addr
	.word 0
SYM_DATA_END(test_kprobes_addresses)

    .section .rodata
SYM_DATA_START(test_kprobes_functions)
	.word test_kprobes_j
	.word test_kprobes_jal
	.word test_kprobes_jr
	.word test_kprobes_jalr
	.word test_kprobes_branch
	.word test_kprobes_adrp
	.word 0
SYM_DATA_END(test_kprobes_functions)
