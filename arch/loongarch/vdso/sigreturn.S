/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Author: Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */

#include <asm/vdso/vdso.h>

#include <linux/linkage.h>
#include <uapi/asm/unistd.h>

#include <asm/regdef.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>

	.section	.text

	.cfi_startproc
	.cfi_signal_frame

	/*
	 * There is a struct rt_sigframe at $sp, set CFA to the address of
	 * the struct sigcontext in the rt_sigframe to simplify the
	 * offsets below.
	 */
	.cfi_def_cfa 3, RT_SIGFRAME_SC

	/*
	 * 72 is DWARF 2 CFA column for the return address from a signal
	 * handler context on LoongArch, i.e. the PC stored in the
	 * sigcontext.
	 */
	.cfi_return_column 72
	.cfi_offset 72, SC_PC

	/* The GPRs of the "caller" are also stored in the sigcontext.  */
.irp	num, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, \
	     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
	.cfi_offset \num, SC_REGS + \num * SZREG
.endr

	/*
	 * HACK: The dwarf2 unwind routine will subtract 1 from the return
	 * address to get an address in the middle of the persumed call
	 * instruction.  While in libgcc there exists a logic to avoid
	 * subtracting 1 for the signal frame (a frame with the 'S'
	 * augmentation that we've already added via .cfi_signal_frame),
	 * unfortunately it doesn't really work: the check of signal frame
	 * is at libgcc/unwind-dw2:1008 in GCC 15.2.0, but the flag it
	 * checks will only get updated by the extract_cie_info call at line
	 * 1025.  So include a nop before the real start to make up for it.
	 * This is also the reason we don't use SYM_FUNC_START.
	 */
	nop
SYM_START(__vdso_rt_sigreturn, SYM_L_GLOBAL, SYM_A_ALIGN)
	li.w	a7, __NR_rt_sigreturn
	syscall	0
	.cfi_endproc
SYM_END(__vdso_rt_sigreturn, SYM_T_FUNC)
