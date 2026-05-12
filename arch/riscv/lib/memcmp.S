/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

/* int memcmp(const void *cs, const void *ct, size_t n) */
SYM_FUNC_START(memcmp)

	__ALTERNATIVE_CFG("nop", "j memcmp_zbb", 0, RISCV_ISA_EXT_ZBB,
		IS_ENABLED(CONFIG_RISCV_ISA_ZBB) && IS_ENABLED(CONFIG_TOOLCHAIN_HAS_ZBB))
/*
 * Parameters
 *	a0 - Pointer to first memory block (cs), also return value
 *	a1 - Pointer to second memory block (ct)
 *	a2 - Number of bytes to compare (n), transformed to end pointer (a0 + n)
 *
 * Returns
 *	a0 - 0 if equal, positive if cs > ct, negative if cs < ct
 *
 * Clobbers
 *	t0, t1
 */
	beqz	a2, 2f
	add	a2, a0, a2
1:
	lbu	t0, 0(a0)
	lbu	t1, 0(a1)
	bne	t0, t1, 3f
	addi	a0, a0, 1
	addi	a1, a1, 1
	bne	a0, a2, 1b
2:
	li	a0, 0
	ret
3:
	sub	a0, t0, t1
	ret


memcmp_zbb:
.option push
.option arch,+zbb
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
 *	t0, t1, t2
 */
	beq	a0, a1, 4f

	li	t0, SZREG
	bltu	a2, t0, 5f

1:
	REG_L	t1, 0(a0)
	REG_L	t2, 0(a1)
	bne	t1, t2, 2f

	addi	a0, a0, SZREG
	addi	a1, a1, SZREG
	addi	a2, a2, -SZREG
	bgeu	a2, t0, 1b

5:
	beqz	a2, 4f
6:
	lbu	t1, 0(a0)
	lbu	t2, 0(a1)
	bne	t1, t2, 3f
	addi	a0, a0, 1
	addi	a1, a1, 1
	addi	a2, a2, -1
	bnez	a2, 6b

4:	li	a0, 0
	ret
2:
#ifndef CONFIG_CPU_BIG_ENDIAN
	rev8	t1, t1
	rev8	t2, t2
#endif
	sltu	a0, t2, t1
	sltu	t0, t1, t2
	sub	a0, a0, t0
	ret

3:
	sub	a0, t1, t2
	ret

.option pop

SYM_FUNC_END(memcmp)
SYM_FUNC_ALIAS(__pi_memcmp, memcmp)
EXPORT_SYMBOL(memcmp)
