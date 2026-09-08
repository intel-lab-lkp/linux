/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  S390 version
 *
 *  Derived from "include/asm-i386/elf.h"
 */

#ifndef __ASMS390_ELF_H
#define __ASMS390_ELF_H

/*
 * HWCAP flags - for AT_HWCAP
 *
 * Bits 32-63 are reserved for use by libc.
 * Bit 31 is reserved and will be used by libc to determine if a second
 * argument is passed to IFUNC resolvers. This will be implemented when
 * there is a need for AT_HWCAP2.
 */
enum {
	HWCAP_NR_ESAN3		= 0,
	HWCAP_NR_ZARCH		= 1,
	HWCAP_NR_STFLE		= 2,
	HWCAP_NR_MSA		= 3,
	HWCAP_NR_LDISP		= 4,
	HWCAP_NR_EIMM		= 5,
	HWCAP_NR_DFP		= 6,
	HWCAP_NR_HPAGE		= 7,
	HWCAP_NR_ETF3EH		= 8,
	HWCAP_NR_HIGH_GPRS	= 9,
	HWCAP_NR_TE		= 10,
	HWCAP_NR_VXRS		= 11,
	HWCAP_NR_VXRS_BCD	= 12,
	HWCAP_NR_VXRS_EXT	= 13,
	HWCAP_NR_GS		= 14,
	HWCAP_NR_VXRS_EXT2	= 15,
	HWCAP_NR_VXRS_PDE	= 16,
	HWCAP_NR_SORT		= 17,
	HWCAP_NR_DFLT		= 18,
	HWCAP_NR_VXRS_PDE2	= 19,
	HWCAP_NR_NNPA		= 20,
	HWCAP_NR_PCI_MIO	= 21,
	HWCAP_NR_SIE		= 22,
	HWCAP_NR_MAX
};

/* Bits present in AT_HWCAP. */
#define HWCAP_ESAN3		BIT(HWCAP_NR_ESAN3)
#define HWCAP_ZARCH		BIT(HWCAP_NR_ZARCH)
#define HWCAP_STFLE		BIT(HWCAP_NR_STFLE)
#define HWCAP_MSA		BIT(HWCAP_NR_MSA)
#define HWCAP_LDISP		BIT(HWCAP_NR_LDISP)
#define HWCAP_EIMM		BIT(HWCAP_NR_EIMM)
#define HWCAP_DFP		BIT(HWCAP_NR_DFP)
#define HWCAP_HPAGE		BIT(HWCAP_NR_HPAGE)
#define HWCAP_ETF3EH		BIT(HWCAP_NR_ETF3EH)
#define HWCAP_HIGH_GPRS		BIT(HWCAP_NR_HIGH_GPRS)
#define HWCAP_TE		BIT(HWCAP_NR_TE)
#define HWCAP_VXRS		BIT(HWCAP_NR_VXRS)
#define HWCAP_VXRS_BCD		BIT(HWCAP_NR_VXRS_BCD)
#define HWCAP_VXRS_EXT		BIT(HWCAP_NR_VXRS_EXT)
#define HWCAP_GS		BIT(HWCAP_NR_GS)
#define HWCAP_VXRS_EXT2		BIT(HWCAP_NR_VXRS_EXT2)
#define HWCAP_VXRS_PDE		BIT(HWCAP_NR_VXRS_PDE)
#define HWCAP_SORT		BIT(HWCAP_NR_SORT)
#define HWCAP_DFLT		BIT(HWCAP_NR_DFLT)
#define HWCAP_VXRS_PDE2		BIT(HWCAP_NR_VXRS_PDE2)
#define HWCAP_NNPA		BIT(HWCAP_NR_NNPA)
#define HWCAP_PCI_MIO		BIT(HWCAP_NR_PCI_MIO)
#define HWCAP_SIE		BIT(HWCAP_NR_SIE)

/*
 * These are used to set parameters in the core dumps.
 */
#define ELF_CLASS	ELFCLASS64
#define ELF_DATA	ELFDATA2MSB
#define ELF_ARCH	EM_S390

/*
 * ELF register definitions..
 */

#include <asm/ptrace.h>
#include <asm/syscall.h>
#include <asm/user.h>

typedef s390_fp_regs elf_fpregset_t;
typedef s390_regs elf_gregset_t;

#include <linux/sched/mm.h>	/* for task_struct */
#include <asm/mmu_context.h>

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) \
	(((x)->e_machine == EM_S390 || (x)->e_machine == EM_S390_OLD) \
         && (x)->e_ident[EI_CLASS] == ELF_CLASS) 

/* For SVR4/S390 the function pointer to be registered with `atexit` is
   passed in R14. */
#define ELF_PLAT_INIT(_r, load_addr) \
	do { \
		_r->gprs[14] = 0; \
	} while (0)

#define CORE_DUMP_USE_REGSET
#define ELF_EXEC_PAGESIZE	PAGE_SIZE

/* This is the location that an ET_DYN program is loaded if exec'ed.  Typical
   use of this is to invoke "./ld.so someprog" to test out a new version of
   the loader.  We need to make sure that it is out of the way of the program
   that it will "exec", and that there is sufficient room for the brk. 64-bit
   tasks are aligned to 4GB. */
#define ELF_ET_DYN_BASE ((STACK_TOP / 3 * 2) & ~((1UL << 32) - 1))

/* This yields a mask that user programs can use to figure out what
   instruction set this CPU supports. */

extern unsigned long elf_hwcap;
#define ELF_HWCAP (elf_hwcap)

/* This yields a string that ld.so will use to load implementation
   specific libraries for optimization.  This is more specific in
   intent than poking at uname or /proc/cpuinfo.

   For the moment, we have only optimizations for the Intel generations,
   but that could change... */

#define ELF_PLATFORM_SIZE 8
extern char elf_platform[];
#define ELF_PLATFORM (elf_platform)

#define SET_PERSONALITY(ex) \
do {								\
	set_personality(PER_LINUX |				\
		(current->personality & (~PER_MASK)));		\
} while (0)

/*
 * Cache aliasing on the latest machines calls for a mapping granularity
 * of 512KB for the anonymous mapping base. Use a 512KB alignment and a
 * randomization of up to 1GB.
 * For the additional randomization of the program break use 32MB.
 */
#define BRK_RND_MASK	(0x1fffUL)
#define MMAP_RND_MASK	(0x3ff80UL)
#define MMAP_ALIGN_MASK	(0x7fUL)
#define STACK_RND_MASK	MMAP_RND_MASK

/* update AT_VECTOR_SIZE_ARCH if the number of NEW_AUX_ENT entries changes */
#define ARCH_DLINFO							\
do {									\
	NEW_AUX_ENT(AT_SYSINFO_EHDR,					\
		    (unsigned long)current->mm->context.vdso_base);	\
} while (0)

struct linux_binprm;

#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
int arch_setup_additional_pages(struct linux_binprm *, int);

#endif
