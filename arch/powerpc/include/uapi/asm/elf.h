/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * ELF register definitions..
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef _UAPI_ASM_POWERPC_ELF_H
#define _UAPI_ASM_POWERPC_ELF_H


#include <linux/types.h>

#include <asm/ptrace.h>
#include <asm/cputable.h>
#include <asm/auxvec.h>


#define ELF_NGREG	48	/* includes nip, msr, lr, etc. */
#define ELF_NFPREG	33	/* includes fpscr */
#define ELF_NVMX	34	/* includes all vector registers */
#define ELF_NVSX	32	/* includes all VSX registers */
#define ELF_NTMSPRREG	3	/* include tfhar, tfiar, texasr */
#define ELF_NEBB	3	/* includes ebbrr, ebbhr, bescr */
#define ELF_NPMU	5	/* includes siar, sdar, sier, mmcr2, mmcr0 */
#define ELF_NPKEY	3	/* includes amr, iamr, uamor */
#define ELF_NDEXCR	2	/* includes dexcr, hdexcr */
#define ELF_NHASHKEYR	1	/* includes hashkeyr */

typedef unsigned long elf_greg_t64;
typedef elf_greg_t64 elf_gregset_t64[ELF_NGREG];

typedef unsigned int elf_greg_t32;
typedef elf_greg_t32 elf_gregset_t32[ELF_NGREG];
typedef elf_gregset_t32 compat_elf_gregset_t;

/*
 * ELF_ARCH, CLASS, and DATA are used to set parameters in the core dumps.
 */
#ifdef __powerpc64__
# define ELF_NVRREG32	33	/* includes vscr & vrsave stuffed together */
# define ELF_NVRREG	34	/* includes vscr & vrsave in split vectors */
# define ELF_NVSRHALFREG 32	/* Half the vsx registers */
# define ELF_GREG_TYPE	elf_greg_t64
# define ELF_ARCH	EM_PPC64
# define ELF_CLASS	ELFCLASS64
typedef elf_greg_t64 elf_greg_t;
typedef elf_gregset_t64 elf_gregset_t;
#else
# define ELF_NEVRREG	34	/* includes acc (as 2) */
# define ELF_NVRREG	33	/* includes vscr */
# define ELF_GREG_TYPE	elf_greg_t32
# define ELF_ARCH	EM_PPC
# define ELF_CLASS	ELFCLASS32
typedef elf_greg_t32 elf_greg_t;
typedef elf_gregset_t32 elf_gregset_t;
#endif /* __powerpc64__ */

#ifdef __BIG_ENDIAN__
#define ELF_DATA	ELFDATA2MSB
#else
#define ELF_DATA	ELFDATA2LSB
#endif

/* Floating point registers */
typedef double elf_fpreg_t;
typedef elf_fpreg_t elf_fpregset_t[ELF_NFPREG];

/* Altivec registers */
/*
 * The entries with indexes 0-31 contain the corresponding vector registers. 
 * The entry with index 32 contains the vscr as the last word (offset 12) 
 * within the quadword.  This allows the vscr to be stored as either a 
 * quadword (since it must be copied via a vector register to/from storage) 
 * or as a word.  
 *
 * 64-bit kernel notes: The entry at index 33 contains the vrsave as the first  
 * word (offset 0) within the quadword.
 *
 * This definition of the VMX state is compatible with the current PPC32 
 * ptrace interface.  This allows signal handling and ptrace to use the same 
 * structures.  This also simplifies the implementation of a bi-arch 
 * (combined (32- and 64-bit) gdb.
 *
 * Note that it's _not_ compatible with 32 bits ucontext which stuffs the
 * vrsave along with vscr and so only uses 33 vectors for the register set
 */
typedef __vector128 elf_vrreg_t;
typedef elf_vrreg_t elf_vrregset_t[ELF_NVRREG];
#ifdef __powerpc64__
typedef elf_vrreg_t elf_vrregset_t32[ELF_NVRREG32];
typedef elf_fpreg_t elf_vsrreghalf_t32[ELF_NVSRHALFREG];
#endif

#endif /* _UAPI_ASM_POWERPC_ELF_H */
