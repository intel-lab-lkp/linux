/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BLOG argument packing.
 */
#ifndef _FS_CEPH_BLOG_SER_H
#define _FS_CEPH_BLOG_SER_H

#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define IS_STR_PTR(t) \
	(__builtin_types_compatible_p(typeof(t), const char *) || \
	 __builtin_types_compatible_p(typeof(t), char *) || \
	 __builtin_types_compatible_p(typeof(t), const unsigned char *) || \
	 __builtin_types_compatible_p(typeof(t), unsigned char *))

#define IS_STR_ARRAY(t) \
	(__builtin_types_compatible_p(typeof(t), const char []) || \
	 __builtin_types_compatible_p(typeof(t), char []) || \
	 __builtin_types_compatible_p(typeof(t), const unsigned char []) || \
	 __builtin_types_compatible_p(typeof(t), unsigned char []))

#define IS_STR(t) (IS_STR_PTR(t) || IS_STR_ARRAY(t))

struct blog_bounded_string {
	const char *str;
	size_t len;
};

#define BLOG_STR(__str, __len) \
	(&(const struct blog_bounded_string){ \
		.str = (const char *)(__str), \
		.len = (__len), \
	})

#define IS_BOUNDED_STR(t) \
	(__builtin_types_compatible_p(typeof(t), \
				      const struct blog_bounded_string *) || \
	 __builtin_types_compatible_p(typeof(t), struct blog_bounded_string *))

#define __suppress_cast_warning(type, value) \
({ \
	_Pragma("GCC diagnostic push") \
	_Pragma("GCC diagnostic ignored \"-Wint-to-pointer-cast\"") \
	_Pragma("GCC diagnostic ignored \"-Wpointer-to-int-cast\"") \
	type __scw_result; \
	__scw_result = ((type)(value)); \
	_Pragma("GCC diagnostic pop") \
	__scw_result; \
})

#define ___blog_concat(__a, __b) __a ## __b
#define ___blog_apply(__fn, __n) ___blog_concat(__fn, __n)

#define ___blog_nth(_, __1, __2, __3, __4, __5, __6, __7, __8, __9, \
	__10, __11, __12, __13, __14, __15, __16, __17, __18, __19, __20, \
	__21, __22, __23, __24, __25, __26, __27, __28, __29, __30, __31, \
	__32, __N, ...) __N
#define ___blog_narg(...) ___blog_nth(_, ##__VA_ARGS__, \
	32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, \
	16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define blog_narg(...) ___blog_narg(__VA_ARGS__)

#define STR_MAX_SIZE 255
#define BLOG_MAX_ARGS 32

/**
 * struct blog_arg - an evaluated argument and its serialization reservation
 * @value: cached scalar value
 * @str: cached string pointer
 * @string_len: measured number of source bytes, excluding the terminator
 * @reserved: bytes reserved for this argument, including string padding
 * @is_string: @str and @string_len are valid instead of @value
 *
 * Logging macros build these records before reserving pagefrag space.  String
 * expressions are therefore evaluated once, and serialization cannot copy
 * beyond the length used to calculate that string's reservation.
 */
struct blog_arg {
	union {
		u64 value;
		const char *str;
	};
	u16 string_len;
	u16 reserved;
	bool is_string;
};

static inline void blog_arg_set_string(struct blog_arg *arg, const char *str,
				       size_t limit)
{
	size_t len = 0;

	arg->is_string = true;
	arg->str = str;
	if (!str) {
		arg->string_len = 0;
		arg->reserved = sizeof("(NULL) ");
		return;
	}

	/*
	 * @limit is the caller's scan bound.  Unbounded %s passes
	 * STR_MAX_SIZE; BLOG_STR() passes the caller length (paths,
	 * NAME_MAX dentries) and must not be silently shrunk to 254.
	 * Cap only so reserved (= round_up(len + 1, 4)) fits in u16.
	 */
	limit = min_t(size_t, limit, (size_t)U16_MAX - 4);
	while (len < limit && str[len])
		len++;
	arg->string_len = len;
	arg->reserved = round_up(len + 1, 4);
}

#define const_char_ptr(str) __suppress_cast_warning(const char *, (str))
/* Integer-width round-trip so both if-branches type-check for any arg. */
#define bounded_string_ptr(str) \
	((const struct blog_bounded_string *)(unsigned long)(str))

#define BLOG_ARG(__arg) \
({ \
	__auto_type __blog_value = (__arg); \
	struct blog_arg __blog_arg = {}; \
	if (IS_BOUNDED_STR(__blog_value)) { \
		const struct blog_bounded_string *__blog_str = \
			bounded_string_ptr(__blog_value); \
		blog_arg_set_string(&__blog_arg, __blog_str->str, \
				    __blog_str->len); \
	} else if (IS_STR(__blog_value)) { \
		blog_arg_set_string(&__blog_arg, const_char_ptr(__blog_value), \
				    STR_MAX_SIZE); \
	} else { \
		/* Same-width integer first so 32-bit sparse does not see \
		 * pointer-to-u64. Wider scalars still go through u64. \
		 */ \
		if (sizeof(__blog_value) == sizeof(void *)) \
			__blog_arg.value = (u64)__suppress_cast_warning( \
				unsigned long, __blog_value); \
		else \
			__blog_arg.value = __suppress_cast_warning( \
				u64, __blog_value); \
		__blog_arg.reserved = sizeof(__blog_value) < 4 ? \
			4 : sizeof(__blog_value); \
	} \
	__blog_arg; \
})

#define ___blog_args0()
#define ___blog_args1(__t) BLOG_ARG(__t)
#define ___blog_args2(__t, __args...) BLOG_ARG(__t), ___blog_args1(__args)
#define ___blog_args3(__t, __args...) BLOG_ARG(__t), ___blog_args2(__args)
#define ___blog_args4(__t, __args...) BLOG_ARG(__t), ___blog_args3(__args)
#define ___blog_args5(__t, __args...) BLOG_ARG(__t), ___blog_args4(__args)
#define ___blog_args6(__t, __args...) BLOG_ARG(__t), ___blog_args5(__args)
#define ___blog_args7(__t, __args...) BLOG_ARG(__t), ___blog_args6(__args)
#define ___blog_args8(__t, __args...) BLOG_ARG(__t), ___blog_args7(__args)
#define ___blog_args9(__t, __args...) BLOG_ARG(__t), ___blog_args8(__args)
#define ___blog_args10(__t, __args...) BLOG_ARG(__t), ___blog_args9(__args)
#define ___blog_args11(__t, __args...) BLOG_ARG(__t), ___blog_args10(__args)
#define ___blog_args12(__t, __args...) BLOG_ARG(__t), ___blog_args11(__args)
#define ___blog_args13(__t, __args...) BLOG_ARG(__t), ___blog_args12(__args)
#define ___blog_args14(__t, __args...) BLOG_ARG(__t), ___blog_args13(__args)
#define ___blog_args15(__t, __args...) BLOG_ARG(__t), ___blog_args14(__args)
#define ___blog_args16(__t, __args...) BLOG_ARG(__t), ___blog_args15(__args)
#define ___blog_args17(__t, __args...) BLOG_ARG(__t), ___blog_args16(__args)
#define ___blog_args18(__t, __args...) BLOG_ARG(__t), ___blog_args17(__args)
#define ___blog_args19(__t, __args...) BLOG_ARG(__t), ___blog_args18(__args)
#define ___blog_args20(__t, __args...) BLOG_ARG(__t), ___blog_args19(__args)
#define ___blog_args21(__t, __args...) BLOG_ARG(__t), ___blog_args20(__args)
#define ___blog_args22(__t, __args...) BLOG_ARG(__t), ___blog_args21(__args)
#define ___blog_args23(__t, __args...) BLOG_ARG(__t), ___blog_args22(__args)
#define ___blog_args24(__t, __args...) BLOG_ARG(__t), ___blog_args23(__args)
#define ___blog_args25(__t, __args...) BLOG_ARG(__t), ___blog_args24(__args)
#define ___blog_args26(__t, __args...) BLOG_ARG(__t), ___blog_args25(__args)
#define ___blog_args27(__t, __args...) BLOG_ARG(__t), ___blog_args26(__args)
#define ___blog_args28(__t, __args...) BLOG_ARG(__t), ___blog_args27(__args)
#define ___blog_args29(__t, __args...) BLOG_ARG(__t), ___blog_args28(__args)
#define ___blog_args30(__t, __args...) BLOG_ARG(__t), ___blog_args29(__args)
#define ___blog_args31(__t, __args...) BLOG_ARG(__t), ___blog_args30(__args)
#define ___blog_args32(__t, __args...) BLOG_ARG(__t), ___blog_args31(__args)
#define BLOG_ARGS(...) \
	___blog_apply(___blog_args, blog_narg(__VA_ARGS__))(__VA_ARGS__)

/*
 * Write args into a caller-supplied array (the per-task TLS scratch).
 * Do not use a compound literal / VLA here: that is what blew VFS
 * stack frames when boutc() inlined the old CEPH_BLOG_LOG_CLIENT.
 */
#define ___blog_fill0(__dst, ...) do { } while (0)
#define ___blog_fill1(__dst, _a0) do { (__dst)[0] = BLOG_ARG(_a0); } while (0)
#define ___blog_fill2(__dst, _a0, _a1) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); } while (0)
#define ___blog_fill3(__dst, _a0, _a1, _a2) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); } while (0)
#define ___blog_fill4(__dst, _a0, _a1, _a2, _a3) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); } while (0)
#define ___blog_fill5(__dst, _a0, _a1, _a2, _a3, _a4) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); } while (0)
#define ___blog_fill6(__dst, _a0, _a1, _a2, _a3, _a4, _a5) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); } while (0)
#define ___blog_fill7(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); } while (0)
#define ___blog_fill8(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); } while (0)
#define ___blog_fill9(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); } while (0)
#define ___blog_fill10(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); } while (0)
#define ___blog_fill11(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); } while (0)
#define ___blog_fill12(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); } while (0)
#define ___blog_fill13(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); } while (0)
#define ___blog_fill14(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); } while (0)
#define ___blog_fill15(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); } while (0)
#define ___blog_fill16(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); } while (0)
#define ___blog_fill17(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); } while (0)
#define ___blog_fill18(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); } while (0)
#define ___blog_fill19(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); } while (0)
#define ___blog_fill20(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); } while (0)
#define ___blog_fill21(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); } while (0)
#define ___blog_fill22(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); } while (0)
#define ___blog_fill23(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); } while (0)
#define ___blog_fill24(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); } while (0)
#define ___blog_fill25(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); } while (0)
#define ___blog_fill26(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); } while (0)
#define ___blog_fill27(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); } while (0)
#define ___blog_fill28(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26, _a27) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); (__dst)[27] = BLOG_ARG(_a27); } while (0)
#define ___blog_fill29(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26, _a27, _a28) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); (__dst)[27] = BLOG_ARG(_a27); (__dst)[28] = BLOG_ARG(_a28); } while (0)
#define ___blog_fill30(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26, _a27, _a28, _a29) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); (__dst)[27] = BLOG_ARG(_a27); (__dst)[28] = BLOG_ARG(_a28); (__dst)[29] = BLOG_ARG(_a29); } while (0)
#define ___blog_fill31(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26, _a27, _a28, _a29, _a30) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); (__dst)[27] = BLOG_ARG(_a27); (__dst)[28] = BLOG_ARG(_a28); (__dst)[29] = BLOG_ARG(_a29); (__dst)[30] = BLOG_ARG(_a30); } while (0)
#define ___blog_fill32(__dst, _a0, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8, _a9, _a10, _a11, _a12, _a13, _a14, _a15, _a16, _a17, _a18, _a19, _a20, _a21, _a22, _a23, _a24, _a25, _a26, _a27, _a28, _a29, _a30, _a31) do { (__dst)[0] = BLOG_ARG(_a0); (__dst)[1] = BLOG_ARG(_a1); (__dst)[2] = BLOG_ARG(_a2); (__dst)[3] = BLOG_ARG(_a3); (__dst)[4] = BLOG_ARG(_a4); (__dst)[5] = BLOG_ARG(_a5); (__dst)[6] = BLOG_ARG(_a6); (__dst)[7] = BLOG_ARG(_a7); (__dst)[8] = BLOG_ARG(_a8); (__dst)[9] = BLOG_ARG(_a9); (__dst)[10] = BLOG_ARG(_a10); (__dst)[11] = BLOG_ARG(_a11); (__dst)[12] = BLOG_ARG(_a12); (__dst)[13] = BLOG_ARG(_a13); (__dst)[14] = BLOG_ARG(_a14); (__dst)[15] = BLOG_ARG(_a15); (__dst)[16] = BLOG_ARG(_a16); (__dst)[17] = BLOG_ARG(_a17); (__dst)[18] = BLOG_ARG(_a18); (__dst)[19] = BLOG_ARG(_a19); (__dst)[20] = BLOG_ARG(_a20); (__dst)[21] = BLOG_ARG(_a21); (__dst)[22] = BLOG_ARG(_a22); (__dst)[23] = BLOG_ARG(_a23); (__dst)[24] = BLOG_ARG(_a24); (__dst)[25] = BLOG_ARG(_a25); (__dst)[26] = BLOG_ARG(_a26); (__dst)[27] = BLOG_ARG(_a27); (__dst)[28] = BLOG_ARG(_a28); (__dst)[29] = BLOG_ARG(_a29); (__dst)[30] = BLOG_ARG(_a30); (__dst)[31] = BLOG_ARG(_a31); } while (0)
#define BLOG_FILL_ARGS(__dst, ...) \
	___blog_apply(___blog_fill, blog_narg(__VA_ARGS__))(__dst, __VA_ARGS__)

static inline size_t blog_args_size(const struct blog_arg *args,
				    size_t nr_args)
{
	size_t size = 0;
	size_t i;

	for (i = 0; i < nr_args; i++)
		size += args[i].reserved;
	return size;
}

static inline size_t blog_serialize_string(char *dst,
					   const struct blog_arg *arg)
{
	static const char null_str[] = "(NULL) ";
	size_t limit;
	size_t count;

	if (!arg->str) {
		memcpy(dst, null_str, min(sizeof(null_str), arg->reserved));
		return arg->reserved;
	}

	limit = min(arg->string_len, arg->reserved - 1);
	for (count = 0; count < limit; count++) {
		dst[count] = arg->str[count];
		if (!dst[count])
			return round_up(count + 1, 4);
	}
	dst[count] = '\0';
	return round_up(count + 1, 4);
}

static inline void *blog_serialize_args(void *buffer,
					const struct blog_arg *args,
					size_t nr_args)
{
	char *dst = buffer;
	size_t i;

	for (i = 0; i < nr_args; i++) {
		const struct blog_arg *arg = &args[i];

		if (arg->is_string) {
			dst += blog_serialize_string(dst, arg);
		} else if (arg->reserved == 8) {
			put_unaligned(arg->value, (u64 *)dst);
			dst += 8;
		} else {
			put_unaligned((u32)arg->value, (u32 *)dst);
			dst += 4;
		}
	}
	return dst;
}

#endif /* _FS_CEPH_BLOG_SER_H */
