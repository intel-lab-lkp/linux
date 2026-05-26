/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 FORTH-ICS/CARV
 *  Nick Kossifidis <mick@ics.forth.gr>
 */

#include <asm/asm.h>	/* For RISCV_* and REG_* macros */
#include <asm/csr.h>	/* For CSR_* macros */
#include <asm/page.h>	/* For PAGE_SIZE */
#include <linux/linkage.h> /* For SYM_* macros */

.section ".rodata"
SYM_CODE_START(riscv_kexec_relocate)

	/*
	 * s0: Pointer to the current entry
	 * s1: (const) Phys address to jump to after relocation
	 * s2: (const) Phys address of the FDT image
	 * s3: (const) The hartid of the current hart
	 * s4: (const) kernel_map.va_pa_offset, used when switching MMU off
	 * s5: Pointer to the destination address for the relocation
	 * s6: (const) Physical address of the main loop
	 */
	mv	s0, a0
	mv	s1, a1
	mv	s2, a2
	mv	s3, a3
	mv	s4, a4
	mv	s5, zero
	mv	s6, zero

	/* Disable / cleanup interrupts */
	csrw	CSR_SIE, zero
	csrw	CSR_SIP, zero

	/*
	 * The trampoline wrapper (riscv_kexec_relocate_entry) has already
	 * dropped the MMU and handed control to us at this PA copy of the
	 * relocate code. From here on the entire loop runs with SATP=0 and
	 * every address (s0, s5, source/dest pointers) is a physical one.
	 */

	/* Process entries in a loop */
1:
	REG_L	t0, 0(s0)		/* t0 = *image->entry */
	addi	s0, s0, RISCV_SZPTR	/* image->entry++ */

	/* IND_DESTINATION entry ? -> save destination address */
	andi	t1, t0, 0x1
	beqz	t1, 2f
	andi	s5, t0, ~0x1
	j	1b

2:
	/* IND_INDIRECTION entry ? -> update next entry ptr (PA) */
	andi	t1, t0, 0x2
	beqz	t1, 2f
	andi	s0, t0, ~0x2
	/* MMU is already off; the entry wrapper handled the transition. */
	j	1b

2:
	/* IND_DONE entry ? -> jump to done label */
	andi	t1, t0, 0x4
	beqz	t1, 2f
	j	4f

2:
	/*
	 * IND_SOURCE entry ? -> copy page word by word to the
	 * destination address we got from IND_DESTINATION
	 */
	andi	t1, t0, 0x8
	beqz	t1, 1b		/* Unknown entry type, ignore it */
	andi	t0, t0, ~0x8
	li	t3, (PAGE_SIZE / RISCV_SZPTR)	/* i = num words per page */
3:	/* copy loop */
	REG_L	t1, (t0)	/* t1 = *src_ptr */
	REG_S	t1, (s5)	/* *dst_ptr = *src_ptr */
	addi	t0, t0, RISCV_SZPTR /* stc_ptr++ */
	addi	s5, s5, RISCV_SZPTR /* dst_ptr++ */
	addi	t3, t3, -0x1	/* i-- */
	beqz	t3, 1b		/* copy done ? */
	j	3b

4:
	/* Pass the arguments to the next kernel  / Cleanup*/
	mv	a0, s3
	mv	a1, s2
	mv	a2, s1

	/* Cleanup */
	mv	a3, zero
	mv	a4, zero
	mv	a5, zero
	mv	a6, zero
	mv	a7, zero

	mv	s0, zero
	mv	s1, zero
	mv	s2, zero
	mv	s3, zero
	mv	s4, zero
	mv	s5, zero
	mv	s6, zero
	mv	s7, zero
	mv	s8, zero
	mv	s9, zero
	mv	s10, zero
	mv	s11, zero

	mv	t0, zero
	mv	t1, zero
	mv	t2, zero
	mv	t3, zero
	mv	t4, zero
	mv	t5, zero
	mv	t6, zero
	csrw	CSR_SEPC, zero
	csrw	CSR_SCAUSE, zero
	csrw	CSR_SSCRATCH, zero

	/*
	 * Make sure the relocated code is visible
	 * and jump to the new kernel
	 */
	fence.i

	jr	a2

SYM_CODE_END(riscv_kexec_relocate)
riscv_kexec_relocate_end:


/* Used for jumping to crashkernel */
.extern kexec_tramp_satp
.extern riscv_kexec_norelocate_pa
.section ".kexec.tramp.text", "ax"
SYM_CODE_START(riscv_kexec_norelocate)
	/*
	 * Two-pass entry:
	 *   - 1st entry: t3 == 0 (initialized by machine_kexec()).
	 *
	 *   - 2nd entry: t3 holds the physical address of
	 *     riscv_kexec_norelocate, so auipc matches t3 and we fall through
	 *     to label 1 to continue execution under trampoline VA(=PA).
	 */
	auipc   t0, 0
	beq     t0, t3, 1f

	la      t0, riscv_kexec_norelocate_pa
	REG_L   t3, 0(t0)
	la      t0, kexec_tramp_satp
	REG_L   t1, 0(t0)
	csrw	CSR_SATP, t1
	sfence.vma x0, x0

	jr      t3
	/*
	 * s0: (const) Phys address to jump to
	 * s1: (const) Phys address of the FDT image
	 * s2: (const) The hartid of the current hart
	 */
1:
	mv	s0, a1
	mv	s1, a2
	mv	s2, a3

	/* Disable / cleanup interrupts */
	csrw	CSR_SIE, zero
	csrw	CSR_SIP, zero

	/* Pass the arguments to the next kernel  / Cleanup*/
	mv	a0, s2
	mv	a1, s1
	mv	a2, s0

	/* Cleanup */
	mv	a3, zero
	mv	a4, zero
	mv	a5, zero
	mv	a6, zero
	mv	a7, zero

	mv	s0, zero
	mv	s1, zero
	mv	s2, zero
	mv	s3, zero
	mv	s4, zero
	mv	s5, zero
	mv	s6, zero
	mv	s7, zero
	mv	s8, zero
	mv	s9, zero
	mv	s10, zero
	mv	s11, zero

	mv	t0, zero
	mv	t1, zero
	mv	t2, zero
	mv	t3, zero
	mv	t4, zero
	mv	t5, zero
	mv	t6, zero
	csrw	CSR_SEPC, zero
	csrw	CSR_SCAUSE, zero
	csrw	CSR_SSCRATCH, zero

	/*
	 * We are already executing from the trampoline VA with the trampoline
	 * page table installed, so there is no need to rely on the old flow
	 * of programming stvec and taking the implicit trap on SATP switch.
	 * Jump directly to the target entry instead.
	 */
	csrw	CSR_SATP, zero
	jr	a2

SYM_CODE_END(riscv_kexec_norelocate)

.extern riscv_kexec_relocate_entry_pa
.extern riscv_kexec_cc_buffer_pa
.section ".kexec.tramp.text", "ax"
SYM_CODE_START(riscv_kexec_relocate_entry)
	/*
	 * Two-pass entry, identical in shape to riscv_kexec_norelocate:
	 *   - 1st entry: t3 == 0 (initialized by machine_kexec()).
	 *   - 2nd entry: t3 == PA of riscv_kexec_relocate_entry, so auipc
	 *                matches t3 and we fall through to label 1.
	 * Args a0..a4 are passed through unchanged to riscv_kexec_relocate.
	 */
	auipc	t0, 0
	beq	t0, t3, 1f

	la	t0, riscv_kexec_relocate_entry_pa
	REG_L	t3, 0(t0)
	la	t0, kexec_tramp_satp
	REG_L	t1, 0(t0)
	csrw	CSR_SATP, t1
	sfence.vma x0, x0

	jr	t3
1:
	/*
	 * Now executing at the PA of this wrapper with the trampoline pgd
	 * installed (identity-mapped). Drop the MMU; PC stays valid because
	 * it is already a PA.
	 */
	csrw	CSR_SATP, zero
	sfence.vma x0, x0

	/* Jump to the PA of control_code_buffer to run the relocate body. */
	la	t0, riscv_kexec_cc_buffer_pa
	REG_L	t0, 0(t0)
	jr	t0
SYM_CODE_END(riscv_kexec_relocate_entry)

.section ".rodata"
SYM_DATA(riscv_kexec_relocate_size,
	.long riscv_kexec_relocate_end - riscv_kexec_relocate)

