// SPDX-License-Identifier: GPL-2.0
/* System call table for x86-64. */

#include <asm/syscall.h>

struct pt_regs;
typedef long (*sys_call_ptr_t)(const struct pt_regs *);
extern const sys_call_ptr_t sys_call_table[];
#if defined(CONFIG_X86_32)
#define ia32_sys_call_table sys_call_table
#else
/*
 * These may not exist, but still put the prototypes in so we
 * can use IS_ENABLED().
 */
extern const sys_call_ptr_t ia32_sys_call_table[];
extern const sys_call_ptr_t x32_sys_call_table[];
#endif

#define __SYSCALL(nr, sym) extern long __x64_##sym(const struct pt_regs *);
#include <asm/syscalls_64.h>
#undef __SYSCALL

#define __SYSCALL(nr, sym) __x64_##sym,

asmlinkage const sys_call_ptr_t sys_call_table[] = {
#include <asm/syscalls_64.h>
};
