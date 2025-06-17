/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_UNALIGNED_H
#define __VDSO_UNALIGNED_H

#define ____get_unaligned_type(type) type: (type)0
/**
 * __get_unaligned_t - read an unaligned value from memory.
 * @ptr:	the pointer to load from.
 * @type:	the type to load from the pointer.
 *
 * Use memcpy to affect an unaligned type sized load avoiding undefined behavior
 * from approaches like type punning that require -fno-strict-aliasing in order
 * to be correct. As type may be const, use _Generic to map to a non-const type
 * - you can't memcpy into a const type. The void* cast silences ubsan warnings.
 */
#define __get_unaligned_t(type, ptr) ({					\
	type __get_unaligned_map_ctrl = 0;				\
	typeof(_Generic(__get_unaligned_map_ctrl,			\
		____get_unaligned_type(short int),			\
		____get_unaligned_type(unsigned short int),		\
		____get_unaligned_type(int),				\
		____get_unaligned_type(unsigned int),			\
		____get_unaligned_type(long),				\
		____get_unaligned_type(unsigned long),			\
		____get_unaligned_type(long long),			\
		____get_unaligned_type(unsigned long long),		\
		default: (type)0					\
		)) __get_unaligned_val;					\
	(void)__get_unaligned_map_ctrl;					\
	__builtin_memcpy(&__get_unaligned_val, (void *)(ptr),		\
			 sizeof(__get_unaligned_val));			\
	__get_unaligned_val;						\
})

/**
 * __put_unaligned_t - write an unaligned value to memory.
 * @type:	the type of the value to store.
 * @val:	the value to store.
 * @ptr:	the pointer to store to.
 *
 * Use memcpy to affect an unaligned type sized store avoiding undefined
 * behavior from approaches like type punning that require -fno-strict-aliasing
 * in order to be correct. The void* cast silences ubsan warnings.
 */
#define __put_unaligned_t(type, val, ptr) do {				\
	type __put_unaligned_val = (val);				\
	__builtin_memcpy((void *)(ptr), &__put_unaligned_val,		\
			 sizeof(__put_unaligned_val));			\
} while (0)

#endif /* __VDSO_UNALIGNED_H */
