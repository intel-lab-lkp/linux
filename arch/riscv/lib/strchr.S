/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Copyright (C) 2025 Feng Jiang <jiangfeng@kylinos.cn>
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

/* char *strchr(const char *s, int c) */
SYM_FUNC_START(strchr)

	__ALTERNATIVE_CFG("nop", "j strchr_zbb", 0, RISCV_ISA_EXT_ZBB,
		IS_ENABLED(CONFIG_RISCV_ISA_ZBB) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZBB))

	/*
	 * Parameters
	 *   a0 - The string to be searched
	 *   a1 - The character to search for
	 *
	 * Returns
	 *   a0 - Address of first occurrence of 'c' or 0
	 *
	 * Clobbers
	 *   t0
	 */
	andi	a1, a1, 0xff
1:
	lbu	t0, 0(a0)
	beq	t0, a1, 2f
	addi	a0, a0, 1
	bnez	t0, 1b
	li	a0, 0
2:
	ret

/*
 * Variant of strchr using the ZBB extension if available
 *
 * This implementation uses orc.b to detect both NUL terminators and target
 * characters in parallel, processing word-sized chunks for efficiency.
 */
#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
strchr_zbb:

#ifdef CONFIG_CPU_BIG_ENDIAN
# define CZ	clz
#else
# define CZ	ctz
#endif

.option push
.option arch,+zbb

	/*
	 * Returns
	 *   a0 - Address of first occurrence of 'c' or NULL
	 *
	 * Parameters
	 *   a0 - String to search
	 *   a1 - Character to find
	 *
	 * Clobbers
	 *   t0, t1, t2, t3, t4
	 */

	/*
	 * Prepare target character mask.
	 * Broadcast target character to all bytes using multiply.
	 */
	andi	a1, a1, 0xff
	li	t1, 0x01010101
#if __riscv_xlen == 64
	slli	t2, t1, 32
	or	t1, t1, t2
#endif
	mul	t2, a1, t1

	/* All-ones mask for orc.b comparisons. */
	li	t4, -1

	/* Check alignment: jump out-of-line if misaligned. */
	andi	t0, a0, SZREG-1
	bnez	t0, 10f

	/* Entry Path A: Directly aligned. Pre-bias for the loop. */
	addi	a0, a0, -SZREG

2:
	/* Main loop: process word-sized chunks. */
	REG_L	t0, SZREG(a0)
	addi	a0, a0, SZREG

	/* Check for NUL terminator. */
	orc.b	t1, t0
	bne	t1, t4, 3f

	/* Check for target character. */
	xor	t1, t0, t2
	orc.b	t1, t1
	beq	t1, t4, 2b

	/* Target found in chunk without NUL. */
	not	t1, t1
	CZ	t1, t1
	srli	t1, t1, 3
	add	a0, a0, t1
	ret

3:
	/* NUL found in current chunk. Check if target appears before NUL. */
	not	t1, t1

	xor	t3, t0, t2
	orc.b	t3, t3
	not	t3, t3

	CZ	t3, t3
	CZ	t1, t1

	/* If NUL appears before target, character not found. */
	bltu	t1, t3, 8f

	srli	t3, t3, 3
	add	a0, a0, t3
	ret

8:
	/* Character not found, return NULL. */
	li	a0, 0
9:
	ret

	/* --- Out-of-line: Handle misaligned portion byte-by-byte --- */
10:
	lbu	t1, 0(a0)
	beq	t1, a1, 9b
	beqz	t1, 8b
	addi	a0, a0, 1
	andi	t0, a0, SZREG-1
	bnez	t0, 10b

	/* Entry Path B: Misalignment fixed. Pre-bias and enter main loop. */
	addi	a0, a0, -SZREG
	j	2b

.option pop
#endif
SYM_FUNC_END(strchr)

SYM_FUNC_ALIAS_WEAK(__pi_strchr, strchr)
EXPORT_SYMBOL(strchr)
