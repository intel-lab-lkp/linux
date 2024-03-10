/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_TYPES_H
#define __LINUX_COMPILER_TYPES_H

/*
 * __has_builtin is supported on gcc >= 10, clang >= 3 and icc >= 21.
 * In the meantime, to support gcc < 10, we implement __has_builtin
 * by hand.
 */
#ifndef __has_builtin
#define __has_builtin(x) (0)
#endif

/* sparse defines __CHECKER__; see Documentation/dev-tools/sparse.rst */
#ifdef __CHECKER__
/* address spaces */
# define __kernel	__attribute__((address_space(0)))
# define __user		__attribute__((noderef, address_space(__user)))
# define __iomem	__attribute__((noderef, address_space(__iomem)))
# define __percpu	__attribute__((noderef, address_space(__percpu)))
# define __rcu		__attribute__((noderef, address_space(__rcu)))
static inline void __chk_user_ptr(const volatile void __user *ptr) { }
static inline void __chk_io_ptr(const volatile void __iomem *ptr) { }
/* context/locking */
# define __must_hold(x)	__attribute__((context(x,1,1)))
# define __acquires(x)	__attribute__((context(x,0,1)))
# define __cond_acquires(x) __attribute__((context(x,0,-1)))
# define __releases(x)	__attribute__((context(x,1,0)))
# define __acquire(x)	__context__(x,1)
# define __release(x)	__context__(x,-1)
# define __cond_lock(x,c)	((c) ? ({ __acquire(x); 1; }) : 0)
/* other */
# define __force	__attribute__((force))
# define __nocast	__attribute__((nocast))
# define __safe		__attribute__((safe))
# define __private	__attribute__((noderef))
# define ACCESS_PRIVATE(p, member) (*((typeof((p)->member) __force *) &(p)->member))
#else /* __CHECKER__ */
/* address spaces */
# define __kernel
# ifdef STRUCTLEAK_PLUGIN
#  define __user	__attribute__((user))
# else
//#  define __user	BTF_TYPE_TAG(user)
# endif
# define __iomem
# define __percpu	BTF_TYPE_TAG(percpu)
# define __rcu		BTF_TYPE_TAG(rcu)

# define __chk_user_ptr(x)	(void)0
# define __chk_io_ptr(x)	(void)0
/* context/locking */
# define __must_hold(x)
# define __acquires(x)
# define __cond_acquires(x)
# define __releases(x)
# define __acquire(x)	(void)0
# define __release(x)	(void)0
# define __cond_lock(x,c) (c)
/* other */
# define __force
# define __nocast
# define __safe
# define __private
# define ACCESS_PRIVATE(p, member) ((p)->member)
# define __builtin_warning(x, y...) (1)
#endif /* __CHECKER__ */

/* Indirect macros required for expanded argument pasting, eg. __LINE__. */
#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)

/* Attributes */
#include <linux/compiler_attributes.h>

/* Compiler specific macros. */
#ifdef __clang__
#include <linux/compiler-clang.h>
#elif defined(__GNUC__)
/* The above compilers also define __GNUC__, so order is important here. */
#include <linux/compiler-gcc.h>
#else
#error "Unknown compiler"
#endif

/*
 * Any place that could be marked with the "alloc_size" attribute is also
 * a place to be marked with the "malloc" attribute, except those that may
 * be performing a _reallocation_, as that may alias the existing pointer.
 * For these, use __realloc_size().
 */
#ifdef __alloc_size__
# define __alloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__) __malloc
# define __realloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__)
#else
# define __alloc_size(x, ...)	__malloc
# define __realloc_size(x, ...)
#endif

/*
 * When the size of an allocated object is needed, use the best available
 * mechanism to find it. (For cases where sizeof() cannot be used.)
 */
#if __has_builtin(__builtin_dynamic_object_size)
#define __struct_size(p)	__builtin_dynamic_object_size(p, 0)
#define __member_size(p)	__builtin_dynamic_object_size(p, 1)
#else
#define __struct_size(p)	__builtin_object_size(p, 0)
#define __member_size(p)	__builtin_object_size(p, 1)
#endif

/* Are two types/vars the same type (ignoring qualifiers)? */
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *			       non-scalar types unchanged.
 */
/*
 * Prefer C11 _Generic for better compile-times and simpler code. Note: 'char'
 * is not type-compatible with 'signed char', and we define a separate case.
 */
#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,			\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(char)0,				\
			 __scalar_type_to_expr_cases(char),		\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),		\
			 __scalar_type_to_expr_cases(long),		\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))

/* Is this type a native word size -- useful for atomic operations */
#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#ifndef NDEBUG
# ifndef __cplusplus
#  pragma GCC diagnostic ignored "-Wnested-externs"
# endif
# define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {								\
		/*							\
		 * __noreturn is needed to give the compiler enough	\
		 * information to avoid certain possibly-uninitialized	\
		 * warnings (regardless of the build failing).		\
		 */							\
		__noreturn extern void prefix ## suffix(void)		\
			__compiletime_error(msg);			\
		if (!(condition))					\
			prefix ## suffix();				\
	} while (0)
#else
# define __compiletime_assert(condition, msg, prefix, suffix) do { } while (0)
#endif

#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)

/**
 * compiletime_assert - break build and emit msg if condition is false
 * @condition: a compile-time constant condition to check
 * @msg:       a message to emit if condition is false
 *
 * In tradition of POSIX assert, this macro will break the build if the
 * supplied condition is *false*, emitting the supplied error message if the
 * compiler has support to do so.
 */
#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __COUNTER__)

#define compiletime_assert_atomic_type(t)				\
	compiletime_assert(__native_word(t),				\
		"Need native word sized stores/loads for atomicity.")

/* Helpers for emitting diagnostics in pragmas. */
#ifndef __diag
#define __diag(string)
#endif

#ifndef __diag_GCC
#define __diag_GCC(version, severity, string)
#endif

#define __diag_push()	__diag(push)
#define __diag_pop()	__diag(pop)

#define __diag_ignore(compiler, version, option, comment) \
	__diag_ ## compiler(version, ignore, option)
#define __diag_warn(compiler, version, option, comment) \
	__diag_ ## compiler(version, warn, option)
#define __diag_error(compiler, version, option, comment) \
	__diag_ ## compiler(version, error, option)

#ifndef __diag_ignore_all
#define __diag_ignore_all(option, comment)
#endif

#endif /* __LINUX_COMPILER_TYPES_H */
