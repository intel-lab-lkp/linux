/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Chen Miao
 *
 * Based on arch/arm/include/asm/jump_label.h
 */
#ifndef __ASM_JUMP_LABEL_H
#define __ASM_JUMP_LABEL_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <asm/insn-def.h>

#define HAVE_JUMP_LABEL_BATCH

#define JUMP_LABEL_NOP_SIZE OPENRISC_INSN_SIZE

/*
 * should aligned 4
 * for jump_label relative
 * entry.code   = nop.addr - . -> return false
 * entry.target = l_yes - .    -> return true
 * entry.key	= key - .
 */
#define JUMP_TABLE_ENTRY(key, label)			\
	".pushsection	__jump_table, \"aw\"	\n\t"	\
	".align 	4 			\n\t"	\
	".long 		1b - ., " label " - .	\n\t"	\
	".long 		" key " - . 		\n\t"	\
	".popsection				\n\t"

#define ARCH_STATIC_BRANCH_ASM(key, label)		\
	".align		4			\n\t"	\
	"1: l.nop				\n\t"	\
	"    l.nop				\n\t"	\
	JUMP_TABLE_ENTRY(key, label)

static __always_inline bool arch_static_branch(struct static_key *const key,
					       const bool branch)
{
	asm goto (ARCH_STATIC_BRANCH_ASM("%0", "%l[l_yes]")
		  ::"i"(&((char *)key)[branch])::l_yes);

	return false;
l_yes:
	return true;
}

#define ARCH_STATIC_BRANCH_JUMP_ASM(key, label)		\
	".align		4			\n\t"	\
	"1: l.j	" label "			\n\t"	\
	"    l.nop				\n\t"	\
	JUMP_TABLE_ENTRY(key, label)

static __always_inline bool
arch_static_branch_jump(struct static_key *const key, const bool branch)
{
	asm goto (ARCH_STATIC_BRANCH_JUMP_ASM("%0", "%l[l_yes]")
		  ::"i"(&((char *)key)[branch])::l_yes);

	return false;
l_yes:
	return true;
}

#endif /* __ASSEMBLY__ */
#endif /* __ASM_JUMP_LABEL_H */
