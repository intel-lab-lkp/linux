/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Macros for issuing an inline system call from the vDSO.
 */

#ifndef X86_ASM_VDSO_SYS_CALL_H
#define X86_ASM_VDSO_SYS_CALL_H

#include <linux/compiler.h>
#include <asm/cpufeatures.h>
#include <asm/alternative.h>

/*
 * Note: only three arguments are currently supported,
 * because there are no constraint letters for r10, r8, r9.
 */
#ifdef CONFIG_X86_64
/* Using dummy output registers instead of clobbers avoids messing up
   user-specified clobbers. */
#define __sys_instr	"syscall"
#define __sys_clobber	"rcx", "r11", "memory"
#define __sys_nr(x,y)	__NR_ ## x
#define __sys_reg1	"rdi"
#define __sys_reg2	"rsi"
#define __sys_reg3	"rdx"
#define __sys_reg4	"r10"
#define __sys_reg5	"r8"
#define __sys_reg6	"r9"
#else
#define __sys_instr	ALTERNATIVE("ds;ds;ds;int $0x80",	\
				    "call __kernel_vsyscall",	\
				    X86_FEATURE_SYSFAST32)
#define __sys_clobber	"memory"
#define __sys_nr(x,y)	__NR_ ## x ## y
#define __sys_reg1	"ebx"
#define __sys_reg2	"ecx"
#define __sys_reg3	"edx"
#define __sys_reg4	"esi"
#define __sys_reg5	"edi"
#define __sys_reg6	"ebp"
#endif

/*
 * Example usage:
 *
 * result = VDSO_SYSCALL3(foo,64,x,y,z);
 *
 * ... calls foo(x,y,z) on 64 bits, and foo64(x,y,z) on 32 bits.
 */
#define _VDSO_SYSCALL(name,suf32,...)					\
	({								\
		long _sys_num_ret = __sys_nr(name,suf32);		\
		asm_inline volatile(					\
			__sys_instr					\
			: "+a" (_sys_num_ret)				\
			: __VA_ARGS__					\
			: __sys_clobber);				\
		_sys_num_ret;						\
	})

#define VDSO_SYSCALL0(name,suf32)					\
	_VDSO_SYSCALL(name,suf32)
#define VDSO_SYSCALL1(name,suf32,a1)					\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1));				\
	})
#define VDSO_SYSCALL2(name,suf32,a1,a2)				\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		register long _sys_arg2 asm(__sys_reg2) = (long)(a2);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1), "r" (_sys_arg2));	\
	})
#define VDSO_SYSCALL3(name,suf32,a1,a2,a3)				\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		register long _sys_arg2 asm(__sys_reg2) = (long)(a2);	\
		register long _sys_arg3 asm(__sys_reg3) = (long)(a3);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1), "r" (_sys_arg2),		\
			      "r" (_sys_arg3));				\
	})
#define VDSO_SYSCALL4(name,suf32,a1,a2,a3,a4)				\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		register long _sys_arg2 asm(__sys_reg2) = (long)(a2);	\
		register long _sys_arg3 asm(__sys_reg3) = (long)(a3);	\
		register long _sys_arg4 asm(__sys_reg4) = (long)(a4);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1), "r" (_sys_arg2),		\
			      "r" (_sys_arg3), "r" (_sys_arg4));	\
	})
#define VDSO_SYSCALL5(name,suf32,a1,a2,a3,a4,a5)			\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		register long _sys_arg2 asm(__sys_reg2) = (long)(a2);	\
		register long _sys_arg3 asm(__sys_reg3) = (long)(a3);	\
		register long _sys_arg4 asm(__sys_reg4) = (long)(a4);	\
		register long _sys_arg5 asm(__sys_reg5) = (long)(a5);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1), "r" (_sys_arg2),		\
			      "r" (_sys_arg3), "r" (_sys_arg4),		\
			      "r" (_sys_arg5));				\
	})
#define VDSO_SYSCALL6(name,suf32,a1,a2,a3,a4,a5,a6)			\
	({								\
		register long _sys_arg1 asm(__sys_reg1) = (long)(a1);	\
		register long _sys_arg2 asm(__sys_reg2) = (long)(a2);	\
		register long _sys_arg3 asm(__sys_reg3) = (long)(a3);	\
		register long _sys_arg4 asm(__sys_reg4) = (long)(a4);	\
		register long _sys_arg5 asm(__sys_reg5) = (long)(a5);	\
		register long _sys_arg6 asm(__sys_reg6) = (long)(a6);	\
		_VDSO_SYSCALL(name,suf32,				\
			      "r" (_sys_arg1), "r" (_sys_arg2),		\
			      "r" (_sys_arg3), "r" (_sys_arg4),		\
			      "r" (_sys_arg5), "r" (_sys_arg6));	\
	})

#endif /* X86_VDSO_SYS_CALL_H */
