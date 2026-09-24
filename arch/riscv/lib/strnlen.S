/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Base on arch/riscv/lib/strlen.S
 *
 * Copyright (C) Feng Jiang <jiangfeng@kylinos.cn>
 */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

/* size_t strnlen(const char *s, size_t count) */
SYM_FUNC_START(strnlen)

	__ALTERNATIVE_CFG("nop", "j strnlen_zbb", 0, RISCV_ISA_EXT_ZBB,
		IS_ENABLED(CONFIG_RISCV_ISA_ZBB) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZBB))


	/*
	 * Returns
	 *   a0 - String length
	 *
	 * Parameters
	 *   a0 - String to measure
	 *   a1 - Max length of string
	 *
	 * Clobbers
	 *   t0, t1, t2
	 */
	addi	t1, a0, -1
	add	t2, a0, a1
1:
	addi	t1, t1, 1
	beq	t1, t2, 2f
	lbu	t0, 0(t1)
	bnez	t0, 1b
2:
	sub	a0, t1, a0
	ret


/*
 * Variant of strnlen using the ZBB extension if available
 */
#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
strnlen_zbb:

#ifdef CONFIG_CPU_BIG_ENDIAN
# define CZ	clz
# define SHIFT	sll
#else
# define CZ	ctz
# define SHIFT	srl
#endif

.option push
.option arch,+zbb

	/*
	 * Returns
	 *   a0 - String length
	 *
	 * Parameters
	 *   a0 - String to measure
	 *   a1 - Max length of string
	 *
	 * Clobbers
	 *   t0, t1, t2, t3, t4, t5, t6
	 */

	/* If maxlen is 0, return 0. */
	beqz	a1, .Lmaxlen

	/* Save original pointer: final length = (current - orig) + offset. */
	mv	t2, a0

	/* Bytes preceding the string in the first word. */
	andi	t6, a0, SZREG-1

	/* Align pointer. */
	andi	t0, a0, -SZREG

	li	t5, SZREG
	sub	t5, t5, t6
	slli	t6, t6, 3

	/* Load and mask the first word. */
	REG_L	t1, 0(t0)
	SHIFT	t1, t1, t6
	orc.b	t1, t1
	not	t1, t1
	CZ	t1, t1

	/* NUL offset inside the first (shifted) word. */
	srli	a0, t1, 3
	minu	a0, a0, a1

	/* If the NUL lies inside the valid bytes of this first chunk, done. */
	bgtu	t5, a0, .Ldone

	/*
	 * Remaining bytes = a1 - t5 (never underflows here).
	 * Word count = ceil(remaining / SZREG) without addi-wrap hazard.
	 */
	sub	t4, a1, t5
	beqz	t4, .Lmaxlen

#if defined(CONFIG_64BIT)
	srli	t1, t4, 3
	andi	t4, t4, 7
#else
	srli	t1, t4, 2
	andi	t4, t4, 3
#endif
	beqz	t4, 1f
	addi	t1, t1, 1
1:
	mv	t4, t1

	li	t3, -1

	/*
	 * Critical loop: exactly one backward branch.
	 * addi t4 is hoisted before orc.b to hide in the load-use latency.
	 */
	.p2align 3
2:
	REG_L	t1, SZREG(t0)
	addi	t0, t0, SZREG
	addi	t4, t4, -1
	orc.b	t1, t1
	bne	t1, t3, .Lfound
	bnez	t4, 2b

.Lmaxlen:
	mv	a0, a1
.Ldone:
	ret

.Lfound:
	not	t1, t1
	CZ	t1, t1
	srli	t1, t1, 3

	/* Length = bytes already passed + offset in final word. */
	sub	a0, t0, t2
	add	a0, a0, t1
	minu	a0, a0, a1
	ret

.option pop
#endif
SYM_FUNC_END(strnlen)
SYM_FUNC_ALIAS(__pi_strnlen, strnlen)
EXPORT_SYMBOL(strnlen)
