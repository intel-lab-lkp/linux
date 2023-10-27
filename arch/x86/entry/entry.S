/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common place for both 32- and 64-bit entry routines.
 */

#include <linux/linkage.h>
#include <asm/export.h>
#include <asm/msr-index.h>
#include <asm/unwind_hints.h>
#include <asm/segment.h>
#include <asm/cache.h>

.pushsection .noinstr.text, "ax"

SYM_FUNC_START(entry_ibpb)
	movl	$MSR_IA32_PRED_CMD, %ecx
	movl	$PRED_CMD_IBPB, %eax
	xorl	%edx, %edx
	wrmsr
	RET
SYM_FUNC_END(entry_ibpb)
/* For KVM */
EXPORT_SYMBOL_GPL(entry_ibpb);

.popsection

.pushsection .entry.text, "ax"

.align L1_CACHE_BYTES, 0xcc
SYM_CODE_START_NOALIGN(mds_verw_sel)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	.word __KERNEL_DS
.align L1_CACHE_BYTES, 0xcc
SYM_CODE_END(mds_verw_sel);
/* For KVM */
EXPORT_SYMBOL_GPL(mds_verw_sel);

.popsection
