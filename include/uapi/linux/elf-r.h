/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_ELF_R_H
#define _UAPI_LINUX_ELF_R_H

/* i386 relocation types */
#define R_386_NONE	0
#define R_386_32	1
#define R_386_PC32	2
#define R_386_GOT32	3
#define R_386_PLT32	4
#define R_386_COPY	5
#define R_386_GLOB_DAT	6
#define R_386_JMP_SLOT	7
#define R_386_RELATIVE	8
#define R_386_GOTOFF	9
#define R_386_GOTPC	10
#define R_386_NUM	11

/* x86-64 relocation types */
#define R_X86_64_NONE		0	/* No reloc */
#define R_X86_64_64		1	/* Direct 64 bit  */
#define R_X86_64_PC32		2	/* PC relative 32 bit signed */
#define R_X86_64_GOT32		3	/* 32 bit GOT entry */
#define R_X86_64_PLT32		4	/* 32 bit PLT address */
#define R_X86_64_COPY		5	/* Copy symbol at runtime */
#define R_X86_64_GLOB_DAT	6	/* Create GOT entry */
#define R_X86_64_JUMP_SLOT	7	/* Create PLT entry */
#define R_X86_64_RELATIVE	8	/* Adjust by program base */
#define R_X86_64_GOTPCREL	9	/* 32 bit signed pc relative
					   offset to GOT */
#define R_X86_64_32		10	/* Direct 32 bit zero extended */
#define R_X86_64_32S		11	/* Direct 32 bit sign extended */
#define R_X86_64_16		12	/* Direct 16 bit zero extended */
#define R_X86_64_PC16		13	/* 16 bit sign extended pc relative */
#define R_X86_64_8		14	/* Direct 8 bit sign extended  */
#define R_X86_64_PC8		15	/* 8 bit sign extended pc relative */
#define R_X86_64_PC64		24	/* Place relative 64-bit signed */

/* arm relocation types */
#define R_ARM_NONE		0
#define R_ARM_PC24		1
#define R_ARM_ABS32		2
#define R_ARM_REL32		3
#define R_ARM_CALL		28
#define R_ARM_JUMP24		29
#define R_ARM_TARGET1		38
#define R_ARM_V4BX		40
#define R_ARM_PREL31		42
#define R_ARM_MOVW_ABS_NC	43
#define R_ARM_MOVT_ABS		44
#define R_ARM_MOVW_PREL_NC	45
#define R_ARM_MOVT_PREL		46
#define R_ARM_ALU_PC_G0_NC	57
#define R_ARM_ALU_PC_G1_NC	59
#define R_ARM_LDR_PC_G2		63

#define R_ARM_THM_CALL		10
#define R_ARM_THM_JUMP24	30
#define R_ARM_THM_MOVW_ABS_NC	47
#define R_ARM_THM_MOVT_ABS	48
#define R_ARM_THM_MOVW_PREL_NC	49
#define R_ARM_THM_MOVT_PREL	50

#endif /* _UAPI_LINUX_ELF_R_H */
