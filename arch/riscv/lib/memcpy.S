/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 */

#include <linux/linkage.h>
#include <asm/asm.h>

/* void *memcpy(void *, const void *, size_t) */
SYM_FUNC_START(__memcpy)
	move t5, a0  /* Preserve return value */

	/* Defer to byte-oriented copy for small sizes */
	sltiu a3, a2, 128
	bnez a3, 4f
	/* Use word-oriented copy only if low-order bits match */
	andi a3, a0, SZREG-1
	andi a4, a1, SZREG-1
	bne a3, a4, 4f

	beqz a3, 2f  /* Skip if already aligned */
	/*
	 * Round to nearest double word-aligned address
	 * greater than or equal to start address
	 */
	andi a3, a1, ~(SZREG-1)
	addi a3, a3, SZREG
	/* Handle initial misalignment */
	sub a4, a3, a1
1:
	lb a5, 0(a1)
	addi a1, a1, 1
	sb a5, 0(t5)
	addi t5, t5, 1
	bltu a1, a3, 1b
	sub a2, a2, a4  /* Update count */

2:
	andi a4, a2, ~((16*SZREG)-1)
	beqz a4, 4f
	add a3, a1, a4
3:
	REG_L a4,       0(a1)
	REG_L a5,   SZREG(a1)
	REG_L a6, 2*SZREG(a1)
	REG_L a7, 3*SZREG(a1)
	REG_L t0, 4*SZREG(a1)
	REG_L t1, 5*SZREG(a1)
	REG_L t2, 6*SZREG(a1)
	REG_L t3, 7*SZREG(a1)
	REG_L t4, 8*SZREG(a1)
	REG_S a4,       0(t5)
	REG_S a5,   SZREG(t5)
	REG_S a6, 2*SZREG(t5)
	REG_S a7, 3*SZREG(t5)
	REG_S t0, 4*SZREG(t5)
	REG_S t1, 5*SZREG(t5)
	REG_S t2, 6*SZREG(t5)
	REG_S t3, 7*SZREG(t5)
	REG_S t4, 8*SZREG(t5)
	REG_L a4, 9*SZREG(a1)
	REG_L a5, 10*SZREG(a1)
	REG_L a6, 11*SZREG(a1)
	REG_L a7, 12*SZREG(a1)
	REG_L t0, 13*SZREG(a1)
	REG_L t1, 14*SZREG(a1)
	REG_L t2, 15*SZREG(a1)
	addi a1, a1, 16*SZREG
	REG_S a4, 9*SZREG(t5)
	REG_S a5, 10*SZREG(t5)
	REG_S a6, 11*SZREG(t5)
	REG_S a7, 12*SZREG(t5)
	REG_S t0, 13*SZREG(t5)
	REG_S t1, 14*SZREG(t5)
	REG_S t2, 15*SZREG(t5)
	addi t5, t5, 16*SZREG
	bltu a1, a3, 3b
	andi a2, a2, (16*SZREG)-1  /* Update count */

4:
	/* Handle trailing misalignment */
	beqz a2, 6f
	add a3, a1, a2

	/* Use word-oriented copy if co-aligned to word boundary */
	or a5, a1, t5
	or a5, a5, a3
	andi a5, a5, 3
	bnez a5, 5f
7:
	lw a4, 0(a1)
	addi a1, a1, 4
	sw a4, 0(t5)
	addi t5, t5, 4
	bltu a1, a3, 7b

	ret

5:
	lb a4, 0(a1)
	addi a1, a1, 1
	sb a4, 0(t5)
	addi t5, t5, 1
	bltu a1, a3, 5b
6:
	ret
SYM_FUNC_END(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi_memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi___memcpy, __memcpy)
