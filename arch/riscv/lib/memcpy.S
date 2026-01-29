/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 */

#include <linux/linkage.h>
#include <asm/asm.h>

/* void *memcpy(void *, const void *, size_t) */
SYM_FUNC_START(__memcpy)
	move t6, a0  /* Preserve return value */

	/* Check alignment first */
	andi a3, t6, SZREG-1
	andi a4, a1, SZREG-1
	bne a3, a4, .Lshifted_copy

	/* Aligned path: defer to byte-oriented copy for small sizes */
	sltiu a5, a2, 128
	bnez a5, 4f

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
	sb a5, 0(t6)
	addi t6, t6, 1
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
	REG_L t5, 9*SZREG(a1)
	REG_S a4,       0(t6)
	REG_S a5,   SZREG(t6)
	REG_S a6, 2*SZREG(t6)
	REG_S a7, 3*SZREG(t6)
	REG_S t0, 4*SZREG(t6)
	REG_S t1, 5*SZREG(t6)
	REG_S t2, 6*SZREG(t6)
	REG_S t3, 7*SZREG(t6)
	REG_S t4, 8*SZREG(t6)
	REG_S t5, 9*SZREG(t6)
	REG_L a4, 10*SZREG(a1)
	REG_L a5, 11*SZREG(a1)
	REG_L a6, 12*SZREG(a1)
	REG_L a7, 13*SZREG(a1)
	REG_L t0, 14*SZREG(a1)
	REG_L t1, 15*SZREG(a1)
	addi a1, a1, 16*SZREG
	REG_S a4, 10*SZREG(t6)
	REG_S a5, 11*SZREG(t6)
	REG_S a6, 12*SZREG(t6)
	REG_S a7, 13*SZREG(t6)
	REG_S t0, 14*SZREG(t6)
	REG_S t1, 15*SZREG(t6)
	addi t6, t6, 16*SZREG
	bltu a1, a3, 3b
	andi a2, a2, (16*SZREG)-1  /* Update count */
	j 4f			/* Skip shifted copy section */

.Lshifted_copy:
	/*
	 * Source and dest have different alignments.
	 * a3 = dest & (SZREG-1), a4 = src & (SZREG-1)
	 * Align destination first, then use shifted word copy.
	 */

	/* For small sizes, just use byte copy */
	sltiu a5, a2, 16
	bnez a5, 4f

	/* If dest is already aligned, skip to shifted loop setup */
	beqz a3, .Ldest_aligned

	/* Calculate bytes needed to align dest: SZREG - a3 */
	neg a5, a3
	addi a5, a5, SZREG
	sub a2, a2, a5		/* Update count */

.Lalign_dest_loop:
	lb a4, 0(a1)
	addi a1, a1, 1
	sb a4, 0(t6)
	addi t6, t6, 1
	addi a5, a5, -1
	bnez a5, .Lalign_dest_loop

.Ldest_aligned:
	/*
	 * Dest is now aligned. Check if we have enough bytes
	 * remaining for word-oriented copy.
	 */
	sltiu a3, a2, SZREG
	bnez a3, 4f

	/*
	 * Calculate shift amounts based on source alignment (distance).
	 * distance = src & (SZREG-1), guaranteed non-zero since we only
	 * reach here when src and dest had different alignments.
	 */
	andi a3, a1, SZREG-1	/* a3 = distance */
	slli a4, a3, 3		/* a4 = distance * 8 (right shift amount) */
	li a5, SZREG*8
	sub a5, a5, a4		/* a5 = SZREG*8 - distance*8 (left shift) */

	/* Align src backwards to word boundary */
	sub a1, a1, a3

	/* Calculate end address: dest + (count rounded down to words) */
	andi a6, a2, ~(SZREG-1)
	add a6, t6, a6		/* a6 = loop end address for dest */

	/* Load first aligned word from source */
	REG_L t0, 0(a1)

.Lshifted_loop:
	REG_L t1, SZREG(a1)	/* Load next aligned word */
	srl t2, t0, a4		/* Shift right: low part from current word */
	mv t0, t1		/* Current = next for next iteration */
	addi a1, a1, SZREG
	addi t6, t6, SZREG
	sll t3, t0, a5		/* Shift left: high part from next word */
	or t2, t2, t3		/* Combine to form output word */
	REG_S t2, -SZREG(t6)	/* Store to aligned dest */
	bltu t6, a6, .Lshifted_loop

	/* Restore src to correct unaligned position */
	add a1, a1, a3
	/* Calculate remaining byte count */
	andi a2, a2, SZREG-1
	/* Fall through to label 4 for remaining bytes */

4:
	/* Handle trailing misalignment */
	beqz a2, 6f
	add a3, a1, a2

	/* Use word-oriented copy if co-aligned to word boundary */
	or a5, a1, t6
	or a5, a5, a3
	andi a5, a5, 3
	bnez a5, 5f
7:
	lw a4, 0(a1)
	addi a1, a1, 4
	sw a4, 0(t6)
	addi t6, t6, 4
	bltu a1, a3, 7b

	ret

5:
	lb a4, 0(a1)
	addi a1, a1, 1
	sb a4, 0(t6)
	addi t6, t6, 1
	bltu a1, a3, 5b
6:
	ret
SYM_FUNC_END(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi_memcpy, __memcpy)
SYM_FUNC_ALIAS(__pi___memcpy, __memcpy)
