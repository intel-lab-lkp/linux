/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Copyright (C) 2025 Feng Jiang <jiangfeng@kylinos.cn>
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

/* char *strrchr(const char *s, int c) */
SYM_FUNC_START(strrchr)
	__ALTERNATIVE_CFG("nop", "j strrchr_zbb", 0, RISCV_ISA_EXT_ZBB,
		IS_ENABLED(CONFIG_RISCV_ISA_ZBB) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZBB))
	/*
	 * Parameters
	 *	a0 - The string to be searched
	 *	a1 - The character to search for
	 *
	 * Returns
	 *	a0 - Address of last occurrence of 'c' or 0
	 *
	 * Clobbers
	 *	t0, t1
	 */
	andi	a1, a1, 0xff
	mv	t1, a0
	li	a0, 0
1:
	lbu	t0, 0(t1)
	bne	t0, a1, 2f
	mv	a0, t1
2:
	addi	t1, t1, 1
	bnez	t0, 1b
	ret

/*
 * Variant of strrchr using the ZBB extension if available
 */
#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
strrchr_zbb:
.option push
.option arch,+zbb
	/*
	 * Parameters
	 *	a0 - The string to be searched
	 *	a1 - The character to search for
	 *
	 * Returns
	 *	a0 - Address of last occurrence of 'c' or 0
	 *
	 * Clobbers
	 *	t0, t1, t2, t3, t4, t5, t6
	 */
	andi	a1, a1, 0xff
	mv	t1, a0
	li	a0, 0
	beqz	a1, .Lfind_end_zbb

	slli	t5, a1, 8
	or	t5, t5, a1
	slli	t2, t5, 16
	or	t5, t5, t2
#if __riscv_xlen == 64
	slli	t2, t5, 32
	or	t5, t5, t2
#endif

	andi	t2, t1, SZREG-1
	bnez	t2, .Lmisaligned_start

.Lmain_loop_pre:
	li	t4, -1

	.balign 16
.Lmain_loop:
	REG_L	t0, 0(t1)
	addi	t1, t1, SZREG
	xor	t6, t0, t5
	orc.b	t2, t0
	orc.b	t6, t6
	and	t3, t2, t6
	beq	t3, t4, .Lmain_loop

	not	t2, t2
	not	t6, t6

	beqz	t2, .Lonly_matches

	addi	t1, t1, -SZREG
	ctz	t3, t2
	sll	t4, t4, t3
	andn	t6, t6, t4
	beqz	t6, .Ldone

	clz	t3, t6
	srli	t3, t3, 3
	xori	t3, t3, SZREG-1
	add	a0, t1, t3
.Ldone:
	ret

.Lonly_matches:
	clz	t3, t6
	srli	t3, t3, 3
	not	t3, t3
	add	a0, t1, t3
	j	.Lmain_loop

.Lfind_end_zbb:
	andi	t2, t1, SZREG-1
	bnez	t2, .Lmisaligned_end_start

.Lfind_end_pre:
	li	t4, -1

	.balign 16
.Lfind_end_loop:
	REG_L	t0, 0(t1)
	addi	t1, t1, SZREG
	orc.b	t2, t0
	beq	t2, t4, .Lfind_end_loop

	addi	t1, t1, -SZREG
	not	t2, t2
	ctz	t3, t2
	srli	t3, t3, 3
	add	a0, t1, t3
	ret

.Lfound_zero:
	mv	a0, t1
	ret
.Lmisaligned_start:
	ori	t2, t1, SZREG-1
	addi	t2, t2, 1
.Lalign_loop:
	lbu	t0, 0(t1)
	beqz	t0, .Ldone
	bne	t0, a1, 1f
	mv	a0, t1
1:
	addi	t1, t1, 1
	bne	t1, t2, .Lalign_loop
	j	.Lmain_loop_pre

.Lmisaligned_end_start:
	ori	t2, t1, SZREG-1
	addi	t2, t2, 1
.Lfind_end_align:
	lbu	t0, 0(t1)
	beqz	t0, .Lfound_zero
	addi	t1, t1, 1
	bne	t1, t2, .Lfind_end_align
	j	.Lfind_end_pre

.option pop
#endif
SYM_FUNC_END(strrchr)
SYM_FUNC_ALIAS_WEAK(__pi_strrchr, strrchr)
EXPORT_SYMBOL(strrchr)
