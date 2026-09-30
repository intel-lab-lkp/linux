/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Macros for OpenRISC asm
 *
 * Linux architectural port borrowing nearly verbatim from
 * LoongArch and Arm. All original copyrights apply as per
 * the original source declaration.
 */

#ifndef __ASM_ASM_H
#define __ASM_ASM_H

#ifdef CONFIG_KPROBES
#define _ASM_NOKPROBE(symbol)			\
	.pushsection "_kprobe_blacklist", "aw";	\
	.long symbol;				\
	.popsection
#else
#define _ASM_NOKPROBE(symbol)
#endif

#endif /* __ASM_ASM_H */
