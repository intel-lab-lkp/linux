/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_SPARC_PERF_REGS_H
#define _UAPI_ASM_SPARC_PERF_REGS_H

/*
 * Mirrors struct pt_regs, with the PC in the slot of the hardwired-zero
 * %g0.  O6 is %sp as held in the register: for a 64-bit stack (odd O6)
 * it is biased by 2047, and the PERF_SAMPLE_STACK_USER dump starts at
 * O6 + 2047.
 */
enum perf_event_sparc_regs {
	PERF_REG_SPARC_PC,
	PERF_REG_SPARC_G1,
	PERF_REG_SPARC_G2,
	PERF_REG_SPARC_G3,
	PERF_REG_SPARC_G4,
	PERF_REG_SPARC_G5,
	PERF_REG_SPARC_G6,
	PERF_REG_SPARC_G7,
	PERF_REG_SPARC_O0,
	PERF_REG_SPARC_O1,
	PERF_REG_SPARC_O2,
	PERF_REG_SPARC_O3,
	PERF_REG_SPARC_O4,
	PERF_REG_SPARC_O5,
	PERF_REG_SPARC_O6,
	PERF_REG_SPARC_O7,
	PERF_REG_SPARC_TSTATE,
	PERF_REG_SPARC_NPC,
	PERF_REG_SPARC_Y,
	PERF_REG_SPARC_MAX
};

#endif /* _UAPI_ASM_SPARC_PERF_REGS_H */
