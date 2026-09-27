// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 *
 * SuperSPARC and SuperSPARC-II implement a 52-bit by 32-bit integer divide
 * and raise illegal_instruction for udiv and sdiv when the dividend {Y, rs1}
 * has significant bits above bit 51. Complete those divides here.
 *
 * Format 3 (op = 2), op3 0x0e udiv, 0x0f sdiv, plus 0x10 for the forms that
 * set %icc. {Y, rs1} / rs2 saturates on overflow and leaves Y alone.
 */

#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>
#include <asm/psr.h>

#include "entry.h"
#include "unimp_32.h"

int try_emulate_div(struct pt_regs *regs, unsigned int insn)
{
	unsigned int op3, rd, rs1, rs2;
	unsigned long r1, r2, result = 0;
	int fault = 0, is_signed, cc, ovf = 0;

	if ((insn >> 30) != 2)
		return 0;
	op3 = (insn >> 19) & 0x3f;
	switch (op3) {
	case 0x0e: case 0x0f: case 0x1e: case 0x1f:
		break;
	default:
		return 0;
	}
	is_signed = op3 & 0x01;
	cc = op3 & 0x10;

	rd = (insn >> 25) & 0x1f;
	rs1 = (insn >> 14) & 0x1f;
	rs2 = (insn & (1 << 13)) ? 0 : (insn & 0x1f);

	unimp_flush_windows(rs1, rs2, rd);

	r1 = unimp_get_reg(rs1, regs, &fault);
	if (insn & (1 << 13)) {			/* i == 1: sign-extended simm13 */
		long simm = insn & 0x1fff;

		if (simm & 0x1000)
			simm |= ~0x1fffL;
		r2 = (unsigned long)simm;
	} else {
		r2 = unimp_get_reg(rs2, regs, &fault);
	}
	if (fault) {
		/* A windowed operand could not be read from the user stack. */
		send_sig_fault(SIGSEGV, SEGV_MAPERR,
			       (void __user *)regs->u_regs[UREG_FP], current);
		return 1;
	}

	if ((unsigned int)r2 == 0) {
		send_sig_fault(SIGFPE, FPE_INTDIV,
			       (void __user *)regs->pc, current);
		return 1;
	}
	if (is_signed) {
		long long num = ((long long)(int)regs->y << 32) |
				(unsigned int)r1;

		/*
		 * INT64_MIN / -1 is undefined in C; the quotient is +2^63,
		 * a positive overflow.
		 */
		if (num == LLONG_MIN && (int)r2 == -1) {
			result = 0x7fffffff;
			ovf = 1;
		} else {
			long long q = num / (int)r2;

			if (q > 2147483647LL) {
				result = 0x7fffffff;
				ovf = 1;
			} else if (q < -2147483647LL - 1) {
				result = 0x80000000;
				ovf = 1;
			} else {
				result = (unsigned long)(unsigned int)q;
			}
		}
	} else {
		unsigned long long num =
			((unsigned long long)(unsigned int)regs->y << 32) |
			(unsigned int)r1;
		unsigned long long q = num / (unsigned int)r2;

		if (q > 0xffffffffULL) {
			result = 0xffffffff;
			ovf = 1;
		} else {
			result = (unsigned long)(unsigned int)q;
		}
	}

	if (unimp_put_reg(rd, result, regs)) {
		send_sig_fault(SIGSEGV, SEGV_MAPERR,
			       (void __user *)regs->u_regs[UREG_FP], current);
		return 1;
	}

	if (cc) {
		unsigned long psr = regs->psr & ~PSR_ICC;

		if (result & 0x80000000)
			psr |= PSR_N;
		if (result == 0)
			psr |= PSR_Z;
		if (ovf)
			psr |= PSR_V;
		regs->psr = psr;
	}

	regs->pc = regs->npc;
	regs->npc += 4;
	return 1;
}
