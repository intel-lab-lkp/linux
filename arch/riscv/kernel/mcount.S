/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2017 Andes Technology Corporation */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <linux/export.h>
#include <asm/asm.h>
#include <asm/csr.h>
#include <asm/unistd.h>
#include <asm/thread_info.h>
#include <asm/asm-offsets.h>
#include <asm/ftrace.h>

	.text

	/*
	 * The call to ftrace_return_to_handler would overwrite the return
	 * register if a0 was not saved.
	 */
	.macro SAVE_RET_ABI_STATE
	addi	sp, sp, -FREGS_SIZE_ON_STACK
	REG_S	ra, FREGS_RA(sp)
	REG_S	s0, FREGS_S0(sp)
	REG_S	a0, FREGS_A0(sp)
	REG_S	a1, FREGS_A1(sp)
	addi	s0, sp, FREGS_SIZE_ON_STACK
	.endm

	.macro RESTORE_RET_ABI_STATE
	REG_L	ra, FREGS_RA(sp)
	REG_L	s0, FREGS_S0(sp)
	REG_L	a0, FREGS_A0(sp)
	REG_L	a1, FREGS_A1(sp)
	addi	sp, sp, FREGS_SIZE_ON_STACK
	.endm

SYM_TYPED_FUNC_START(ftrace_stub)
       .global _mcount
       .set    _mcount, ftrace_stub
	ret
SYM_FUNC_END(ftrace_stub)

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
SYM_TYPED_FUNC_START(ftrace_stub_graph)
	ret
SYM_FUNC_END(ftrace_stub_graph)

SYM_FUNC_START(return_to_handler)
/*
 * On implementing the frame point test, the ideal way is to compare the
 * s0 (frame pointer, if enabled) on entry and the sp (stack pointer) on return.
 * However, the psABI of variable-length-argument functions does not allow this.
 *
 * So alternatively we check the *old* frame pointer position, that is, the
 * value stored in -16(s0) on entry, and the s0 on return.
 */
	SAVE_RET_ABI_STATE
	mv	a0, sp
	call	ftrace_return_to_handler
	mv	a2, a0
	RESTORE_RET_ABI_STATE
	jalr	a2
SYM_FUNC_END(return_to_handler)
#endif

EXPORT_SYMBOL(_mcount)
