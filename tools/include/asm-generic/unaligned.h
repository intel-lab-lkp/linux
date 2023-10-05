/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copied from the kernel sources to tools/perf/:
 */

#ifndef __TOOLS_LINUX_ASM_GENERIC_UNALIGNED_H
#define __TOOLS_LINUX_ASM_GENERIC_UNALIGNED_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"

#define __get_unaligned_t(type, ptr) ({						\
	const struct { type x; } __packed *__pptr = (typeof(__pptr))(ptr);	\
	__pptr->x;								\
})

#define __put_unaligned_t(type, val, ptr) do {					\
	struct { type x; } __packed *__pptr = (typeof(__pptr))(ptr);		\
	__pptr->x = (val);							\
} while (0)

#define get_unaligned(ptr)	__get_unaligned_t(typeof(*(ptr)), (ptr))
#define put_unaligned(val, ptr) __put_unaligned_t(typeof(*(ptr)), (val), (ptr))

static inline u16 get_unaligned_le16(const void *p)
{
	return le16_to_cpu(__get_unaligned_t(__le16, p));
}

static inline u32 get_unaligned_le32(const void *p)
{
	return le32_to_cpu(__get_unaligned_t(__le32, p));
}

static inline u64 get_unaligned_le64(const void *p)
{
	return le64_to_cpu(__get_unaligned_t(__le64, p));
}

#pragma GCC diagnostic pop

#endif /* __TOOLS_LINUX_ASM_GENERIC_UNALIGNED_H */

