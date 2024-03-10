/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TOOLS_LINUX_COMPILER_H_
#define _TOOLS_LINUX_COMPILER_H_

#include <linux/compiler_types.h>

#ifndef likely
# define likely(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
# define unlikely(x)	__builtin_expect(!!(x), 0)
#endif

/* Optimization barrier */
#ifndef barrier
/* The "volatile" is due to gcc bugs */
# define barrier() __asm__ __volatile__("": : :"memory")
#endif

#ifndef barrier_data
/*
 * This version is i.e. to prevent dead stores elimination on @ptr
 * where gcc and llvm may behave differently when otherwise using
 * normal barrier(): while gcc behavior gets along with a normal
 * barrier(), llvm needs an explicit input variable to be assumed
 * clobbered. The issue is as follows: while the inline asm might
 * access any memory it wants, the compiler could have fit all of
 * @ptr into memory registers instead, and since @ptr never escaped
 * from that, it proved that the inline asm wasn't touching any of
 * it. This version works well with both compilers, i.e. we're telling
 * the compiler that the inline asm absolutely may see the contents
 * of @ptr. See also: https://llvm.org/bugs/show_bug.cgi?id=15495
 */
# define barrier_data(ptr) __asm__ __volatile__("": :"r"(ptr) :"memory")
#endif

#ifndef unreachable
# define unreachable() do {		\
	annotate_unreachable();		\
	__builtin_unreachable();	\
} while (0)
#endif

#ifndef __must_be_array
/* &a[0] degrades to a pointer: a different type from an array */
# define __must_be_array(a)	BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))
#endif

#ifndef __is_constexpr
/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
#endif

/*
 * Whether 'type' is a signed type or an unsigned type. Supports scalar types,
 * bool and also pointer types.
 */
#ifndef is_signed_type
# define is_signed_type(type) (((type)(-1)) < (__force type)1)
#endif
#ifndef is_unsigned_type
# define is_unsigned_type(type) (!is_signed_type(type))
#endif

#include <asm/rwonce.h>

#endif /* _TOOLS_LINUX_COMPILER_H */
