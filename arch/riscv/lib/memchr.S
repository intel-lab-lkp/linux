/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

/*  void *memchr(const void *s, int c, size_t n) */
SYM_FUNC_START(memchr)

	__ALTERNATIVE_CFG("nop", "j memchr_zbb", 0, RISCV_ISA_EXT_ZBB,
		IS_ENABLED(CONFIG_RISCV_ISA_ZBB) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZBB))
/*
 *
 * Parameters
 *	a0 - Pointer to memory area (s), also return value
 *	a1 - Character to search for (c)
 *	a2 - Number of bytes to search (n)
 *
 * Returns
 *	a0 - Pointer to the matched character, or NULL (0) if not found
 *
 * Clobbers
 *	t0, t1
 */
	beqz	a2, 3f
	andi	a1, a1, 0xff
	add	t1, a0, a2

1:
	lbu	t0, 0(a0)
	beq	t0, a1, 4f
	addi	a0, a0, 1
	bne	a0, t1, 1b

3:
	li	a0, 0
4:
	ret

#if defined(CONFIG_RISCV_ISA_ZBB) && defined(CONFIG_TOOLCHAIN_HAS_ZBB)
.balign 4
memchr_zbb:
.option push
.option arch, +zbb
/*
 * Parameters
 *	a0 - Pointer to memory area (s), also return value
 *	a1 - Character to search for (c)
 *	a2 - Number of bytes to search (n)
 *
 * Returns
 *	a0 - Pointer to the matched character, or NULL (0) if not found
 *
 * Clobbers
 *	t0, t1, t2, t3, t4, t5, t6
 */
	beqz	a2, 3b
	add	t6, a0, a2
	andi	t3, a1, 0xff

	li	t0, SZREG
	bltu	a2, t0, 8f

#if __riscv_xlen == 64
	li	t0, 0x0101010101010101
#else
	li	t0, 0x01010101
#endif
	mul	a1, t3, t0

	andi	t1, a0, SZREG - 1
	beqz	t1, 5f

1:
	lbu	t2, 0(a0)
	beq	t2, t3, 4b
	addi	a0, a0, 1
	andi	t1, a0, SZREG - 1
	bnez	t1, 1b

5:
	sub	t5, t6, a0
	andi	t5, t5, -SZREG
	add	t5, a0, t5

	li	t4, -1

	beq	a0, t5, 8f

6:
	REG_L	t0, 0(a0)
	xor	t1, t0, a1
	orc.b	t1, t1
	bne	t1, t4, 7f
	addi	a0, a0, SZREG
	bltu	a0, t5, 6b

8:
	beq	a0, t6, 3b

2:
	lbu	t0, 0(a0)
	beq	t0, t3, 4b
	addi	a0, a0, 1
	bltu	a0, t6, 2b

	j	3b

7:
	not	t1, t1
#ifndef CONFIG_CPU_BIG_ENDIAN
	ctz	t1, t1
#else
	clz	t1, t1
#endif
	srli	t1, t1, 3
	add	a0, a0, t1
	ret

.option pop
#endif

SYM_FUNC_END(memchr)
SYM_FUNC_ALIAS(__pi_memchr, memchr)
EXPORT_SYMBOL(memchr)
