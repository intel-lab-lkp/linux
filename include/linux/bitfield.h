/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2004 - 2009 Ivo van Doorn <IvDoorn@gmail.com>
 */

#ifndef _LINUX_BITFIELD_H
#define _LINUX_BITFIELD_H

#include <linux/build_bug.h>
#include <linux/typecheck.h>
#include <asm/byteorder.h>

/*
 * Bitfield access macros
 *
 * FIELD_{GET,PREP} macros take as first parameter shifted mask
 * from which they extract the base mask and shift amount.
 * Mask must be a compilation time constant.
 * field_{get,prep} are variants that take a non-const mask.
 *
 * Example:
 *
 *  #include <linux/bitfield.h>
 *  #include <linux/bits.h>
 *
 *  #define REG_FIELD_A  GENMASK(6, 0)
 *  #define REG_FIELD_B  BIT(7)
 *  #define REG_FIELD_C  GENMASK(15, 8)
 *  #define REG_FIELD_D  GENMASK(31, 16)
 *
 * Get:
 *  a = FIELD_GET(REG_FIELD_A, reg);
 *  b = FIELD_GET(REG_FIELD_B, reg);
 *
 * Set:
 *  reg = FIELD_PREP(REG_FIELD_A, 1) |
 *	  FIELD_PREP(REG_FIELD_B, 0) |
 *	  FIELD_PREP(REG_FIELD_C, c) |
 *	  FIELD_PREP(REG_FIELD_D, 0x40);
 *
 * Modify:
 *  FIELD_MODIFY(REG_FIELD_C, &reg, c);
 */

#define __bf_shf(x) (__builtin_ffsll(x) - 1)

#define __BF_VALIDATE_MASK(mask) \
	(!(mask) || ((mask) & ((mask) + ((mask) & -(mask)))))

#define __BF_FIELD_CHECK_MASK(mask, pfx)			\
do {								\
	BUILD_BUG_ON_MSG(!__builtin_constant_p(mask),		\
			 pfx "mask is not constant");		\
	BUILD_BUG_ON_MSG(__BF_VALIDATE_MASK(mask),		\
			 pfx "mask is zero or not contiguous");	\
} while (0)

#define __BF_FIELD_CHECK_VAL(mask, val, pfx)			\
	BUILD_BUG_ON_MSG(__builtin_constant_p(val) &&		\
			 ~((mask) >> __bf_shf(mask)) & (val),	\
			 pfx "value too large for the field")

#define __BF_FIELD_CHECK_REG(mask, reg, pfx)			\
	BUILD_BUG_ON_MSG(mask + 0U + 0UL + 0ULL >		\
			 ~0ULL >> (64 - 8 * sizeof (reg)),	\
			 pfx "type of reg too small for mask")

#define __BF_FIELD_PREP(mask, val, pfx)		\
({						\
	__BF_FIELD_CHECK_MASK(mask, pfx);	\
	__BF_FIELD_CHECK_VAL(mask, val, pfx);	\
	((val) << __bf_shf(mask)) & (mask);	\
})

#define __BF_FIELD_GET(mask, reg, pfx)		\
({						\
	__BF_FIELD_CHECK_MASK(mask, pfx);	\
	__BF_FIELD_CHECK_REG(mask, reg, pfx);	\
	((reg) & (mask)) >> __bf_shf(mask);	\
})

/**
 * FIELD_MAX() - produce the maximum value representable by a field
 * @mask: shifted mask defining the field's length and position
 *
 * FIELD_MAX() returns the maximum value that can be held in the field
 * specified by @mask.
 */
#define FIELD_MAX(mask)					\
({							\
	__auto_type _mask = mask;			\
	__BF_FIELD_CHECK_MASK(_mask, "FIELD_MAX: ");	\
	(_mask >> __bf_shf(_mask));			\
})

/**
 * FIELD_FIT() - check if value fits in the field
 * @mask: shifted mask defining the field's length and position
 * @val:  value to test against the field
 *
 * Return: true if @val can fit inside @mask, false if @val is too big.
 */
#define FIELD_FIT(mask, val)				\
({							\
	__auto_type _mask = mask;			\
	__auto_type _val = 1 ? (val) : _mask;		\
	__BF_FIELD_CHECK_MASK(_mask, "FIELD_FIT: ");	\
	!((_val << __bf_shf(_mask)) & ~_mask); 		\
})

/**
 * FIELD_PREP() - prepare a bitfield element
 * @mask: shifted mask defining the field's length and position
 * @val:  value to put in the field
 *
 * FIELD_PREP() masks and shifts up the value.  The result should
 * be combined with other fields of the bitfield using logical OR.
 */
#define FIELD_PREP(mask, val)				\
({							\
	__auto_type _mask = mask;			\
	__auto_type _val = 1 ? (val) : _mask;		\
	__BF_FIELD_PREP(_mask, _val, "FIELD_PREP: ");	\
})

/**
 * FIELD_PREP_CONST() - prepare a constant bitfield element
 * @mask: shifted mask defining the field's length and position
 * @val:  value to put in the field
 *
 * FIELD_PREP_CONST() masks and shifts up the value.  The result should
 * be combined with other fields of the bitfield using logical OR.
 *
 * Unlike FIELD_PREP() this is a constant expression and can therefore
 * be used in initializers. Error checking is less comfortable for this
 * version, and non-constant masks cannot be used.
 */
#define FIELD_PREP_CONST(mask, val)				\
(								\
	/* mask must be non-zero and contiguous */		\
	BUILD_BUG_ON_ZERO(__BF_VALIDATE_MASK(mask)) +		\
	/* check if value fits */				\
	BUILD_BUG_ON_ZERO(~((mask) >> __bf_shf(mask)) & (val)) + \
	/* and create the value */				\
	(((typeof(mask))(val) << __bf_shf(mask)) & (mask))	\
)

/**
 * FIELD_GET() - extract a bitfield element
 * @mask: shifted mask defining the field's length and position
 * @reg:  value of entire bitfield
 *
 * FIELD_GET() extracts the field specified by @mask from the
 * bitfield passed in as @reg by masking and shifting it down.
 */
#define FIELD_GET(mask, reg)				\
({							\
	__auto_type _mask = mask;			\
	__auto_type _reg = reg;				\
	__BF_FIELD_GET(_mask, _reg, "FIELD_GET: ");	\
})

/**
 * FIELD_MODIFY() - modify a bitfield element
 * @mask: shifted mask defining the field's length and position
 * @reg_p: pointer to the memory that should be updated
 * @val: value to store in the bitfield
 *
 * FIELD_MODIFY() modifies the set of bits in @reg_p specified by @mask,
 * by replacing them with the bitfield value passed in as @val.
 */
#define FIELD_MODIFY(mask, reg_p, val)						\
({										\
	__auto_type _mask = mask;						\
	__auto_type _reg_p = reg_p;						\
	__auto_type _val = 1 ? (val) : _mask;					\
	__BF_FIELD_CHECK_MASK(_mask, "FIELD_MODIFY: ");				\
	__BF_FIELD_CHECK_VAL(_mask, _val, "FIELD_MODIFY: ");			\
	__BF_FIELD_CHECK_REG(_mask, *_reg_p, "FIELD_MODIFY: ");			\
	*_reg_p = (*_reg_p & ~_mask) | ((_val << __bf_shf(_mask)) & _mask);	\
})

/*
 * Primitives for manipulating bitfields both in host- and fixed-endian.
 *
 * * u32 le32_get_bits(__le32 val, u32 field) extracts the contents of the
 *   bitfield specified by @field in little-endian 32bit object @val and
 *   converts it to host-endian.
 *
 * * void le32p_replace_bits(__le32 *p, u32 v, u32 field) replaces
 *   the contents of the bitfield specified by @field in little-endian
 *   32bit object pointed to by @p with the value of @v.  New value is
 *   given in host-endian and stored as little-endian.
 *
 * * __le32 le32_replace_bits(__le32 old, u32 v, u32 field) is equivalent to
 *   ({__le32 tmp = old; le32p_replace_bits(&tmp, v, field); tmp;})
 *   In other words, instead of modifying an object in memory, it takes
 *   the initial value and returns the modified one.
 *
 * * __le32 le32_encode_bits(u32 v, u32 field) is equivalent to
 *   le32_replace_bits(0, v, field).  In other words, it returns a little-endian
 *   32bit object with the bitfield specified by @field containing the
 *   value of @v and all bits outside that bitfield being zero.
 *
 * Such set of helpers is defined for each of little-, big- and host-endian
 * types; e.g. u64_get_bits(val, field) will return the contents of the bitfield
 * specified by @field in host-endian 64bit object @val, etc.  Of course, for
 * host-endian no conversion is involved.
 *
 * Fields to access are specified as GENMASK() values - an N-bit field
 * starting at bit #M is encoded as GENMASK(M + N - 1, M).  Note that
 * bit numbers refer to endianness of the object we are working with -
 * e.g. GENMASK(11, 0) in __be16 refers to the second byte and the lower
 * 4 bits of the first byte.  In __le16 it would refer to the first byte
 * and the lower 4 bits of the second byte, etc.
 *
 * Field specification must be a constant; __builtin_constant_p() doesn't
 * have to be true for it, but compiler must be able to evaluate it at
 * build time.  If it cannot or if the value does not encode any bitfield,
 * the build will fail.
 *
 * If the value being stored in a bitfield is a constant that does not fit
 * into that bitfield, a warning will be generated at compile time.
 */

extern void __compiletime_error("value doesn't fit into mask")
__field_overflow(void);
extern void __compiletime_error("bad bitfield mask")
__bad_mask(void);
static __always_inline u64 field_multiplier(u64 field)
{
	if (__BF_VALIDATE_MASK(field))
		__bad_mask();
	return field & -field;
}
static __always_inline u64 field_mask(u64 field)
{
	return field / field_multiplier(field);
}
#define field_max(field)	((typeof(field))field_mask(field))
#define ____MAKE_OP(type,base,to,from)					\
static __always_inline __##type __must_check type##_encode_bits(base v, base field)	\
{									\
	if (__builtin_constant_p(v) && (v & ~field_mask(field)))	\
		__field_overflow();					\
	return to((v & field_mask(field)) * field_multiplier(field));	\
}									\
static __always_inline __##type __must_check type##_replace_bits(__##type old,	\
							base val, base field)	\
{									\
	return (old & ~to(field)) | type##_encode_bits(val, field);	\
}									\
static __always_inline void type##p_replace_bits(__##type *p,		\
					base val, base field)		\
{									\
	*p = (*p & ~to(field)) | type##_encode_bits(val, field);	\
}									\
static __always_inline base __must_check type##_get_bits(__##type v, base field)	\
{									\
	return (from(v) & field)/field_multiplier(field);		\
}
#define __MAKE_OP(size)							\
	____MAKE_OP(le##size,u##size,cpu_to_le##size,le##size##_to_cpu)	\
	____MAKE_OP(be##size,u##size,cpu_to_be##size,be##size##_to_cpu)	\
	____MAKE_OP(u##size,u##size,,)
____MAKE_OP(u8,u8,,)
__MAKE_OP(16)
__MAKE_OP(32)
__MAKE_OP(64)
#undef __MAKE_OP
#undef ____MAKE_OP

/* As __bf_shf() but for non-zero variables */
#define __BF_SHIFT(mask) \
	(BITS_PER_TYPE(_mask) <= 32 ? __ffs(_mask) : __ffs64(_mask))

/**
 * field_prep() - prepare a bitfield element
 * @mask: shifted mask defining the field's length and position, must be
 *        non-zero
 * @val:  value to put in the field
 *
 * Return: field value masked and shifted to its final destination
 *
 * field_prep() masks and shifts up the value.  The result should be
 * combined with other fields of the bitfield using logical OR.
 * Unlike FIELD_PREP(), @mask is not limited to a compile-time constant.
 * Typical usage patterns are a value stored in a table, or calculated by
 * shifting a constant by a variable number of bits.
 * If you want to ensure that @mask is a compile-time constant, please use
 * FIELD_PREP() directly instead.
 */
#define field_prep(mask, val)					\
({								\
	__auto_type _mask = mask;				\
	__auto_type _val = 1 ? (val) : _mask;			\
	__builtin_constant_p(_mask) ?				\
		__BF_FIELD_PREP(_mask, _val, "field_prep: ") :	\
		(_val << __BF_SHIFT(_mask)) & _mask;		\
})

/**
 * field_get() - extract a bitfield element
 * @mask: shifted mask defining the field's length and position, must be
 *        non-zero
 * @reg:  value of entire bitfield
 *
 * Return: extracted field value
 *
 * field_get() extracts the field specified by @mask from the
 * bitfield passed in as @reg by masking and shifting it down.
 * Unlike FIELD_GET(), @mask is not limited to a compile-time constant.
 * Typical usage patterns are a value stored in a table, or calculated by
 * shifting a constant by a variable number of bits.
 * If you want to ensure that @mask is a compile-time constant, please use
 * FIELD_GET() directly instead.
 */
#define field_get(mask, reg)					\
({								\
	__auto_type _mask = mask;				\
	__auto_type _reg = reg;					\
	__builtin_constant_p(_mask) ?				\
		__BF_FIELD_GET(_mask, _reg, "field_get: ") :	\
		(_reg & _mask) >> __BF_SHIFT(_mask);		\
})

#endif
