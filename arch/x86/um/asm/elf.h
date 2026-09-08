/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Licensed under the GPL
 */
#ifndef __UM_ELF_X86_H
#define __UM_ELF_X86_H

#include <asm/user.h>
#include <skas.h>

#define CORE_DUMP_USE_REGSET

#ifdef CONFIG_X86_32

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) \
	(((x)->e_machine == EM_386) || ((x)->e_machine == EM_486))

#define ELF_CLASS	ELFCLASS32
#define ELF_DATA        ELFDATA2LSB
#define ELF_ARCH        EM_386

#define ELF_PLAT_INIT(regs, load_addr) do { \
	PT_REGS_BX(regs) = 0; \
	PT_REGS_CX(regs) = 0; \
	PT_REGS_DX(regs) = 0; \
	PT_REGS_SI(regs) = 0; \
	PT_REGS_DI(regs) = 0; \
	PT_REGS_BP(regs) = 0; \
	PT_REGS_AX(regs) = 0; \
} while (0)

/* Shamelessly stolen from include/asm-i386/elf.h */

#define ELF_CORE_COPY_REGS(pr_reg, regs) do {	\
	pr_reg[0] = PT_REGS_BX(regs);		\
	pr_reg[1] = PT_REGS_CX(regs);		\
	pr_reg[2] = PT_REGS_DX(regs);		\
	pr_reg[3] = PT_REGS_SI(regs);		\
	pr_reg[4] = PT_REGS_DI(regs);		\
	pr_reg[5] = PT_REGS_BP(regs);		\
	pr_reg[6] = PT_REGS_AX(regs);		\
	pr_reg[7] = PT_REGS_DS(regs);		\
	pr_reg[8] = PT_REGS_ES(regs);		\
	/* fake once used fs and gs selectors? */	\
	pr_reg[9] = PT_REGS_DS(regs);		\
	pr_reg[10] = PT_REGS_DS(regs);		\
	pr_reg[11] = PT_REGS_SYSCALL_NR(regs);	\
	pr_reg[12] = PT_REGS_IP(regs);		\
	pr_reg[13] = PT_REGS_CS(regs);		\
	pr_reg[14] = PT_REGS_EFLAGS(regs);	\
	pr_reg[15] = PT_REGS_SP(regs);		\
	pr_reg[16] = PT_REGS_SS(regs);		\
} while (0);

#define ELF_PLATFORM_FALLBACK "i586"

#else

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) \
	((x)->e_machine == EM_X86_64)

#define ELF_CLASS	ELFCLASS64
#define ELF_DATA        ELFDATA2LSB
#define ELF_ARCH        EM_X86_64

#define ELF_PLAT_INIT(regs, load_addr)    do { \
	PT_REGS_BX(regs) = 0; \
	PT_REGS_CX(regs) = 0; \
	PT_REGS_DX(regs) = 0; \
	PT_REGS_SI(regs) = 0; \
	PT_REGS_DI(regs) = 0; \
	PT_REGS_BP(regs) = 0; \
	PT_REGS_AX(regs) = 0; \
	PT_REGS_R8(regs) = 0; \
	PT_REGS_R9(regs) = 0; \
	PT_REGS_R10(regs) = 0; \
	PT_REGS_R11(regs) = 0; \
	PT_REGS_R12(regs) = 0; \
	PT_REGS_R13(regs) = 0; \
	PT_REGS_R14(regs) = 0; \
	PT_REGS_R15(regs) = 0; \
} while (0)

#define ELF_CORE_COPY_REGS(pr_reg, _regs)		\
	(pr_reg)[0] = (_regs)->regs.gp[0];			\
	(pr_reg)[1] = (_regs)->regs.gp[1];			\
	(pr_reg)[2] = (_regs)->regs.gp[2];			\
	(pr_reg)[3] = (_regs)->regs.gp[3];			\
	(pr_reg)[4] = (_regs)->regs.gp[4];			\
	(pr_reg)[5] = (_regs)->regs.gp[5];			\
	(pr_reg)[6] = (_regs)->regs.gp[6];			\
	(pr_reg)[7] = (_regs)->regs.gp[7];			\
	(pr_reg)[8] = (_regs)->regs.gp[8];			\
	(pr_reg)[9] = (_regs)->regs.gp[9];			\
	(pr_reg)[10] = (_regs)->regs.gp[10];			\
	(pr_reg)[11] = (_regs)->regs.gp[11];			\
	(pr_reg)[12] = (_regs)->regs.gp[12];			\
	(pr_reg)[13] = (_regs)->regs.gp[13];			\
	(pr_reg)[14] = (_regs)->regs.gp[14];			\
	(pr_reg)[15] = (_regs)->regs.gp[15];			\
	(pr_reg)[16] = (_regs)->regs.gp[16];			\
	(pr_reg)[17] = (_regs)->regs.gp[17];			\
	(pr_reg)[18] = (_regs)->regs.gp[18];			\
	(pr_reg)[19] = (_regs)->regs.gp[19];			\
	(pr_reg)[20] = (_regs)->regs.gp[20];			\
	(pr_reg)[21] = (_regs)->regs.gp[21];			\
	(pr_reg)[22] = (_regs)->regs.gp[22];			\
	(pr_reg)[23] = 0;					\
	(pr_reg)[24] = 0;					\
	(pr_reg)[25] = 0;					\
	(pr_reg)[26] = 0;

#define ELF_PLATFORM_FALLBACK "x86_64"

#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
struct linux_binprm;
extern int arch_setup_additional_pages(struct linux_binprm *bprm,
	int uses_interp);

extern unsigned long um_vdso_addr;
#define AT_SYSINFO_EHDR 33
#define ARCH_DLINFO	NEW_AUX_ENT(AT_SYSINFO_EHDR, um_vdso_addr)

#endif

typedef unsigned long elf_greg_t;

#define ELF_NGREG (sizeof(struct user_regs_struct) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct user_i387_struct elf_fpregset_t;

struct task_struct;

#define ELF_EXEC_PAGESIZE 4096

#define ELF_ET_DYN_BASE (TASK_SIZE / 3 * 2)

extern long elf_aux_hwcap;
#define ELF_HWCAP (elf_aux_hwcap)

extern char *elf_aux_platform;
#define ELF_PLATFORM (elf_aux_platform ?: ELF_PLATFORM_FALLBACK)

#define SET_PERSONALITY(ex) do {} while(0)

#endif
