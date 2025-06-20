/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_BITOPS_ROTATE_H_
#define _ASM_GENERIC_BITOPS_ROTATE_H_

#include <asm/types.h>

/**
 * generic_rol64 - rotate a 64-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u64 generic_rol64(u64 word, unsigned int shift)
{
	return (word << (shift & 63)) | (word >> ((-shift) & 63));
}

/**
 * generic_ror64 - rotate a 64-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u64 generic_ror64(u64 word, unsigned int shift)
{
	return (word >> (shift & 63)) | (word << ((-shift) & 63));
}

/**
 * generic_rol32 - rotate a 32-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u32 generic_rol32(u32 word, unsigned int shift)
{
	return (word << (shift & 31)) | (word >> ((-shift) & 31));
}

/**
 * generic_ror32 - rotate a 32-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u32 generic_ror32(u32 word, unsigned int shift)
{
	return (word >> (shift & 31)) | (word << ((-shift) & 31));
}

/**
 * generic_rol16 - rotate a 16-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u16 generic_rol16(u16 word, unsigned int shift)
{
	return (word << (shift & 15)) | (word >> ((-shift) & 15));
}

/**
 * generic_ror16 - rotate a 16-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u16 generic_ror16(u16 word, unsigned int shift)
{
	return (word >> (shift & 15)) | (word << ((-shift) & 15));
}

/**
 * generic_rol8 - rotate an 8-bit value left
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u8 generic_rol8(u8 word, unsigned int shift)
{
	return (word << (shift & 7)) | (word >> ((-shift) & 7));
}

/**
 * generic_ror8 - rotate an 8-bit value right
 * @word: value to rotate
 * @shift: bits to roll
 */
static __always_inline u8 generic_ror8(u8 word, unsigned int shift)
{
	return (word >> (shift & 7)) | (word << ((-shift) & 7));
}

#ifndef __HAVE_ARCH_ROTATE
#define rol64(word, shift) generic_rol64(word, shift)
#define ror64(word, shift) generic_ror64(word, shift)
#define rol32(word, shift) generic_rol32(word, shift)
#define ror32(word, shift) generic_ror32(word, shift)
#define rol16(word, shift) generic_rol16(word, shift)
#define ror16(word, shift) generic_ror16(word, shift)
#define rol8(word, shift)  generic_rol8(word, shift)
#define ror8(word, shift)  generic_ror8(word, shift)
#endif

#endif /* _ASM_GENERIC_BITOPS_ROTATE_H_ */
