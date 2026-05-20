/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
.option push
.option arch,+zbb
#endif

/* int memcmp(const void *cs, const void *ct, size_t n) */
SYM_FUNC_START(memcmp)
/*
 * Parameters
 *	a0 - Pointer to first memory block (cs), also return value
 *	a1 - Pointer to second memory block (ct)
 *	a2 - Number of bytes to compare (n), decremented during loop
 *
 * Returns
 *	a0 - 0 if equal, positive if cs > ct, negative if cs < ct
 *
 * Clobbers
 *	t0, t1, t2, t3, t4
 */
	add	t3, a0, a2
	or	t0, a0, a1
	andi	t0, t0, (SZREG - 1)
	bnez	t0, 5f

	addi	t4, t3, -SZREG
	bltu	t4, a0, 7f

1:
	REG_L	t1, 0(a0)
	REG_L	t2, 0(a1)
	bne	t1, t2, 2f
	addi	a0, a0, SZREG
	addi	a1, a1, SZREG
	bleu	a0, t4, 1b

7:
#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
	__ALTERNATIVE_CFG("j 5f", "nop", 0, RISCV_ISA_EXT_ZBB,
			  IS_ENABLED(CONFIG_RISCV_ISA_ZBB))

	beq	a0, t3, 4f
	REG_L	t1, 0(a0)
	REG_L	t2, 0(a1)

	sub	t0, t3, a0
	li	t4, SZREG
	sub	t0, t4, t0
	slli	t0, t0, 3

#ifndef CONFIG_CPU_BIG_ENDIAN
	rev8	t1, t1
	rev8	t2, t2
#endif
	srl	t1, t1, t0
	srl	t2, t2, t0

	bne	t1, t2, 8f
	li	a0, 0
	ret
#else
	j	5f
#endif
5:
	beq	a0, t3, 4f
6:
	lbu	t1, 0(a0)
	lbu	t2, 0(a1)
	bne	t1, t2, 3f
	addi	a0, a0, 1
	addi	a1, a1, 1
	bne	a0, t3, 6b

4:	li	a0, 0
	ret
2:
#if !defined(CONFIG_CPU_BIG_ENDIAN) && \
	defined(CONFIG_TOOLCHAIN_HAS_ZBB) && defined(CONFIG_RISCV_ISA_ZBB)
	__ALTERNATIVE_CFG("j 5b", "nop", 0, RISCV_ISA_EXT_ZBB, IS_ENABLED(CONFIG_RISCV_ISA_ZBB))
	rev8	t1, t1
	rev8	t2, t2
#elif !defined(CONFIG_CPU_BIG_ENDIAN)
	j	5b
#endif
8:
	sltu	a0, t2, t1
	sltu	t0, t1, t2
	sub	a0, a0, t0
	ret

3:
	sub	a0, t1, t2
	ret

SYM_FUNC_END(memcmp)
SYM_FUNC_ALIAS(__pi_memcmp, memcmp)
EXPORT_SYMBOL(memcmp)

#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
.option pop
#endif
