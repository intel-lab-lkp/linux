/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Regents of the University of California
 *
 * RISC-V memory copy template. This file is #include'd by memcpy.S and
 * memcpy_mc.S. The includer must define the following macros before the
 * #include:
 *
 *   LOAD_B(reg, offset, ptr)    - byte load from source
 *   STORE_B(reg, offset, ptr)   - byte store to destination
 *   LOAD_REG(reg, offset, ptr)  - SZREG-sized load from source
 *   STORE_REG(reg, offset, ptr) - SZREG-sized store to destination
 *   LOAD_W(reg, offset, ptr)    - 32-bit word load from source
 *   STORE_W(reg, offset, ptr)   - 32-bit word store to destination
 *
 * For MC-aware copies, LOAD_* macros should use fixup with label "6f".
 * The includer must provide the "6:" fixup handler that returns the
 * number of bytes not copied in a0.
 *
 * Register usage (caller must set a0,a1,a2 per standard calling convention):
 *   a0 (a0)   - original destination address (also return value)
 *   a1 (SRC)     - source pointer (advanced during copy)
 *   a2 (COUNT)   - number of bytes to copy / remaining count
 *   t6 (DST)     - working destination pointer
 */

	move t6, a0  /* Preserve return value */

	/* Defer to byte-oriented copy for small sizes */
	sltiu a3, a2, 128
	bnez a3, .Ltail

	/* Use word-oriented copy only if low-order bits match */
	andi a3, t6, SZREG-1
	andi a4, a1, SZREG-1
	bne a3, a4, .Ltail

	beqz a3, .Laligned  /* Skip if already aligned */
	/*
	 * Round to nearest double word-aligned address
	 * greater than or equal to start address
	 */
	andi a3, a1, ~(SZREG-1)
	addi a3, a3, SZREG
	/* Handle initial misalignment */
	sub a4, a3, a1
.Llead_copy:
	LOAD_B a5, 0, a1
	addi a1, a1, 1
	STORE_B a5, 0, t6
	addi t6, t6, 1
	bltu a1, a3, .Llead_copy
	sub a2, a2, a4  /* Update count */

.Laligned:
	andi a4, a2, ~((16*SZREG)-1)
	beqz a4, .Ltail
	add a3, a1, a4
.Lunrolled_loop:
	LOAD_REG a4,       0, a1
	LOAD_REG a5,   SZREG, a1
	LOAD_REG a6, 2*SZREG, a1
	LOAD_REG a7, 3*SZREG, a1
	LOAD_REG t0, 4*SZREG, a1
	LOAD_REG t1, 5*SZREG, a1
	LOAD_REG t2, 6*SZREG, a1
	LOAD_REG t3, 7*SZREG, a1
	LOAD_REG t4, 8*SZREG, a1
	LOAD_REG t5, 9*SZREG, a1
	STORE_REG a4,       0, t6
	STORE_REG a5,   SZREG, t6
	STORE_REG a6, 2*SZREG, t6
	STORE_REG a7, 3*SZREG, t6
	STORE_REG t0, 4*SZREG, t6
	STORE_REG t1, 5*SZREG, t6
	STORE_REG t2, 6*SZREG, t6
	STORE_REG t3, 7*SZREG, t6
	STORE_REG t4, 8*SZREG, t6
	STORE_REG t5, 9*SZREG, t6
	LOAD_REG a4, 10*SZREG, a1
	LOAD_REG a5, 11*SZREG, a1
	LOAD_REG a6, 12*SZREG, a1
	LOAD_REG a7, 13*SZREG, a1
	LOAD_REG t0, 14*SZREG, a1
	LOAD_REG t1, 15*SZREG, a1
	addi a1, a1, 16*SZREG
	STORE_REG a4, 10*SZREG, t6
	STORE_REG a5, 11*SZREG, t6
	STORE_REG a6, 12*SZREG, t6
	STORE_REG a7, 13*SZREG, t6
	STORE_REG t0, 14*SZREG, t6
	STORE_REG t1, 15*SZREG, t6
	addi t6, t6, 16*SZREG
	bltu a1, a3, .Lunrolled_loop
	andi a2, a2, (16*SZREG)-1  /* Update count */

.Ltail:
	/* Handle trailing misalignment */
	beqz a2, .Lcopy_done
	add a3, a1, a2

	/* Use word-oriented copy if co-aligned to word boundary */
	or a5, a1, t6
	or a5, a5, a3
	andi a5, a5, 3
	bnez a5, .Lbyte_tail

.Lword_tail:
	LOAD_W a4, 0, a1
	addi a1, a1, 4
	STORE_W a4, 0, t6
	addi t6, t6, 4
	bltu a1, a3, .Lword_tail
	j .Lcopy_done

.Lbyte_tail:
	LOAD_B a4, 0, a1
	addi a1, a1, 1
	STORE_B a4, 0, t6
	addi t6, t6, 1
	bltu a1, a3, .Lbyte_tail

.Lcopy_done:
	/* Fall through to includer's return or fixup code */
