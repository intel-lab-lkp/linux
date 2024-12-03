/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024, Intel Corporation
 * Copyright (c) 2024, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _LINUX_PACKING_TYPES_H
#define _LINUX_PACKING_TYPES_H

#include <linux/types.h>

/* If you add another packed field type, please update
 * scripts/mod/packed_fields.c to enable compile time sanity checks.
 */

#define GEN_PACKED_FIELD_MEMBERS(__type) \
	__type startbit; \
	__type endbit; \
	__type offset; \
	__type size

/* Small packed field. Use with bit offsets < 256, buffers < 32B and
 * unpacked structures < 256B.
 */
struct packed_field_s {
	GEN_PACKED_FIELD_MEMBERS(u8);
};

/* Medium packed field. Use with bit offsets < 65536, buffers < 8KB and
 * unpacked structures < 64KB.
 */
struct packed_field_m {
	GEN_PACKED_FIELD_MEMBERS(u16);
};

#define PACKED_FIELD(start, end, struct_name, struct_field) \
{ \
	(start), \
	(end), \
	offsetof(struct_name, struct_field), \
	sizeof_field(struct_name, struct_field), \
}

#define CHECK_PACKED_FIELD(field) ({ \
	typeof(field) __f = (field); \
	BUILD_BUG_ON(__f.startbit < __f.endbit); \
	BUILD_BUG_ON(__f.startbit - __f.endbit >= BITS_PER_BYTE * __f.size); \
	BUILD_BUG_ON(__f.size != 1 && __f.size != 2 && \
		     __f.size != 4 && __f.size != 8); \
})


#define CHECK_PACKED_FIELD_OVERLAP(ascending, field1, field2) ({ \
	typeof(field1) _f1 = (field1); typeof(field2) _f2 = (field2); \
	const bool _a = (ascending); \
	BUILD_BUG_ON(_a && _f1.startbit >= _f2.startbit); \
	BUILD_BUG_ON(!_a && _f1.startbit <= _f2.startbit); \
	BUILD_BUG_ON(max(_f1.endbit, _f2.endbit) <= \
		     min(_f1.startbit, _f2.startbit)); \
})

#define CHECK_PACKED_FIELDS_SIZE(fields, pbuflen) ({ \
	typeof(&(fields)[0]) _f = (fields); \
	typeof(pbuflen) _len = (pbuflen); \
	const size_t num_fields = ARRAY_SIZE(fields); \
	BUILD_BUG_ON(!__builtin_constant_p(_len)); \
	BUILD_BUG_ON(_f[0].startbit >= BITS_PER_BYTE * _len); \
	BUILD_BUG_ON(_f[num_fields - 1].startbit >= BITS_PER_BYTE * _len); \
})

/* Do not hand-edit the following packed field check macros!
 *
 * They are generated using scripts/gen_packed_field_checks.c, which may be
 * built via "make scripts_gen_packed_field_checks". If larger macro sizes are
 * needed in the future, please use this program to re-generate the macros and
 * insert them here.
 */

#define CHECK_PACKED_FIELDS_1(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 1); \
	 CHECK_PACKED_FIELD(_f[0]); })

#define CHECK_PACKED_FIELDS_2(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 2); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); })

#define CHECK_PACKED_FIELDS_3(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 3); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); })

#define CHECK_PACKED_FIELDS_4(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 4); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); })

#define CHECK_PACKED_FIELDS_5(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 5); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); })

#define CHECK_PACKED_FIELDS_6(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 6); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); })

#define CHECK_PACKED_FIELDS_7(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 7); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); })

#define CHECK_PACKED_FIELDS_8(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 8); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); })

#define CHECK_PACKED_FIELDS_9(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 9); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); })

#define CHECK_PACKED_FIELDS_10(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 10); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); })

#define CHECK_PACKED_FIELDS_11(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 11); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); })

#define CHECK_PACKED_FIELDS_12(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 12); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); })

#define CHECK_PACKED_FIELDS_13(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 13); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); })

#define CHECK_PACKED_FIELDS_14(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 14); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); })

#define CHECK_PACKED_FIELDS_15(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 15); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); })

#define CHECK_PACKED_FIELDS_16(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 16); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); })

#define CHECK_PACKED_FIELDS_17(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 17); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); })

#define CHECK_PACKED_FIELDS_18(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 18); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); })

#define CHECK_PACKED_FIELDS_19(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 19); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); })

#define CHECK_PACKED_FIELDS_20(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 20); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); })

#define CHECK_PACKED_FIELDS_21(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 21); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); })

#define CHECK_PACKED_FIELDS_22(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 22); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); })

#define CHECK_PACKED_FIELDS_23(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 23); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); })

#define CHECK_PACKED_FIELDS_24(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 24); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); })

#define CHECK_PACKED_FIELDS_25(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 25); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); })

#define CHECK_PACKED_FIELDS_26(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 26); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); })

#define CHECK_PACKED_FIELDS_27(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 27); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); })

#define CHECK_PACKED_FIELDS_28(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 28); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); })

#define CHECK_PACKED_FIELDS_29(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 29); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); })

#define CHECK_PACKED_FIELDS_30(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 30); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); })

#define CHECK_PACKED_FIELDS_31(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 31); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); })

#define CHECK_PACKED_FIELDS_32(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 32); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); })

#define CHECK_PACKED_FIELDS_33(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 33); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); })

#define CHECK_PACKED_FIELDS_34(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 34); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); })

#define CHECK_PACKED_FIELDS_35(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 35); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); })

#define CHECK_PACKED_FIELDS_36(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 36); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); })

#define CHECK_PACKED_FIELDS_37(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 37); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); })

#define CHECK_PACKED_FIELDS_38(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 38); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); })

#define CHECK_PACKED_FIELDS_39(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 39); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); })

#define CHECK_PACKED_FIELDS_40(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 40); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); })

#define CHECK_PACKED_FIELDS_41(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 41); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); })

#define CHECK_PACKED_FIELDS_42(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 42); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); })

#define CHECK_PACKED_FIELDS_43(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 43); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); })

#define CHECK_PACKED_FIELDS_44(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 44); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); })

#define CHECK_PACKED_FIELDS_45(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 45); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); })

#define CHECK_PACKED_FIELDS_46(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 46); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD(_f[45]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[44], _f[45]); })

#define CHECK_PACKED_FIELDS_47(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 47); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD(_f[45]); \
	 CHECK_PACKED_FIELD(_f[46]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[44], _f[45]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[45], _f[46]); })

#define CHECK_PACKED_FIELDS_48(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 48); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD(_f[45]); \
	 CHECK_PACKED_FIELD(_f[46]); \
	 CHECK_PACKED_FIELD(_f[47]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[44], _f[45]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[45], _f[46]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[46], _f[47]); })

#define CHECK_PACKED_FIELDS_49(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 49); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD(_f[45]); \
	 CHECK_PACKED_FIELD(_f[46]); \
	 CHECK_PACKED_FIELD(_f[47]); \
	 CHECK_PACKED_FIELD(_f[48]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[44], _f[45]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[45], _f[46]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[46], _f[47]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[47], _f[48]); })

#define CHECK_PACKED_FIELDS_50(fields) \
	({ typeof(&(fields)[0]) _f = (fields); \
	 BUILD_BUG_ON(ARRAY_SIZE(fields) != 50); \
	 CHECK_PACKED_FIELD(_f[0]); \
	 CHECK_PACKED_FIELD(_f[1]); \
	 CHECK_PACKED_FIELD(_f[2]); \
	 CHECK_PACKED_FIELD(_f[3]); \
	 CHECK_PACKED_FIELD(_f[4]); \
	 CHECK_PACKED_FIELD(_f[5]); \
	 CHECK_PACKED_FIELD(_f[6]); \
	 CHECK_PACKED_FIELD(_f[7]); \
	 CHECK_PACKED_FIELD(_f[8]); \
	 CHECK_PACKED_FIELD(_f[9]); \
	 CHECK_PACKED_FIELD(_f[10]); \
	 CHECK_PACKED_FIELD(_f[11]); \
	 CHECK_PACKED_FIELD(_f[12]); \
	 CHECK_PACKED_FIELD(_f[13]); \
	 CHECK_PACKED_FIELD(_f[14]); \
	 CHECK_PACKED_FIELD(_f[15]); \
	 CHECK_PACKED_FIELD(_f[16]); \
	 CHECK_PACKED_FIELD(_f[17]); \
	 CHECK_PACKED_FIELD(_f[18]); \
	 CHECK_PACKED_FIELD(_f[19]); \
	 CHECK_PACKED_FIELD(_f[20]); \
	 CHECK_PACKED_FIELD(_f[21]); \
	 CHECK_PACKED_FIELD(_f[22]); \
	 CHECK_PACKED_FIELD(_f[23]); \
	 CHECK_PACKED_FIELD(_f[24]); \
	 CHECK_PACKED_FIELD(_f[25]); \
	 CHECK_PACKED_FIELD(_f[26]); \
	 CHECK_PACKED_FIELD(_f[27]); \
	 CHECK_PACKED_FIELD(_f[28]); \
	 CHECK_PACKED_FIELD(_f[29]); \
	 CHECK_PACKED_FIELD(_f[30]); \
	 CHECK_PACKED_FIELD(_f[31]); \
	 CHECK_PACKED_FIELD(_f[32]); \
	 CHECK_PACKED_FIELD(_f[33]); \
	 CHECK_PACKED_FIELD(_f[34]); \
	 CHECK_PACKED_FIELD(_f[35]); \
	 CHECK_PACKED_FIELD(_f[36]); \
	 CHECK_PACKED_FIELD(_f[37]); \
	 CHECK_PACKED_FIELD(_f[38]); \
	 CHECK_PACKED_FIELD(_f[39]); \
	 CHECK_PACKED_FIELD(_f[40]); \
	 CHECK_PACKED_FIELD(_f[41]); \
	 CHECK_PACKED_FIELD(_f[42]); \
	 CHECK_PACKED_FIELD(_f[43]); \
	 CHECK_PACKED_FIELD(_f[44]); \
	 CHECK_PACKED_FIELD(_f[45]); \
	 CHECK_PACKED_FIELD(_f[46]); \
	 CHECK_PACKED_FIELD(_f[47]); \
	 CHECK_PACKED_FIELD(_f[48]); \
	 CHECK_PACKED_FIELD(_f[49]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[0], _f[1]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[1], _f[2]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[2], _f[3]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[3], _f[4]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[4], _f[5]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[5], _f[6]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[6], _f[7]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[7], _f[8]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[8], _f[9]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[9], _f[10]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[10], _f[11]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[11], _f[12]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[12], _f[13]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[13], _f[14]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[14], _f[15]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[15], _f[16]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[16], _f[17]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[17], _f[18]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[18], _f[19]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[19], _f[20]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[20], _f[21]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[21], _f[22]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[22], _f[23]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[23], _f[24]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[24], _f[25]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[25], _f[26]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[26], _f[27]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[27], _f[28]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[28], _f[29]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[29], _f[30]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[30], _f[31]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[31], _f[32]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[32], _f[33]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[33], _f[34]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[34], _f[35]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[35], _f[36]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[36], _f[37]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[37], _f[38]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[38], _f[39]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[39], _f[40]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[40], _f[41]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[41], _f[42]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[42], _f[43]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[43], _f[44]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[44], _f[45]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[45], _f[46]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[46], _f[47]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[47], _f[48]); \
	 CHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[48], _f[49]); })

#define CHECK_PACKED_FIELDS(fields) \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 1, CHECK_PACKED_FIELDS_1(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 2, CHECK_PACKED_FIELDS_2(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 3, CHECK_PACKED_FIELDS_3(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 4, CHECK_PACKED_FIELDS_4(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 5, CHECK_PACKED_FIELDS_5(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 6, CHECK_PACKED_FIELDS_6(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 7, CHECK_PACKED_FIELDS_7(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 8, CHECK_PACKED_FIELDS_8(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 9, CHECK_PACKED_FIELDS_9(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 10, CHECK_PACKED_FIELDS_10(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 11, CHECK_PACKED_FIELDS_11(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 12, CHECK_PACKED_FIELDS_12(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 13, CHECK_PACKED_FIELDS_13(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 14, CHECK_PACKED_FIELDS_14(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 15, CHECK_PACKED_FIELDS_15(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 16, CHECK_PACKED_FIELDS_16(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 17, CHECK_PACKED_FIELDS_17(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 18, CHECK_PACKED_FIELDS_18(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 19, CHECK_PACKED_FIELDS_19(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 20, CHECK_PACKED_FIELDS_20(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 21, CHECK_PACKED_FIELDS_21(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 22, CHECK_PACKED_FIELDS_22(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 23, CHECK_PACKED_FIELDS_23(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 24, CHECK_PACKED_FIELDS_24(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 25, CHECK_PACKED_FIELDS_25(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 26, CHECK_PACKED_FIELDS_26(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 27, CHECK_PACKED_FIELDS_27(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 28, CHECK_PACKED_FIELDS_28(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 29, CHECK_PACKED_FIELDS_29(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 30, CHECK_PACKED_FIELDS_30(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 31, CHECK_PACKED_FIELDS_31(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 32, CHECK_PACKED_FIELDS_32(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 33, CHECK_PACKED_FIELDS_33(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 34, CHECK_PACKED_FIELDS_34(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 35, CHECK_PACKED_FIELDS_35(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 36, CHECK_PACKED_FIELDS_36(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 37, CHECK_PACKED_FIELDS_37(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 38, CHECK_PACKED_FIELDS_38(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 39, CHECK_PACKED_FIELDS_39(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 40, CHECK_PACKED_FIELDS_40(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 41, CHECK_PACKED_FIELDS_41(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 42, CHECK_PACKED_FIELDS_42(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 43, CHECK_PACKED_FIELDS_43(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 44, CHECK_PACKED_FIELDS_44(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 45, CHECK_PACKED_FIELDS_45(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 46, CHECK_PACKED_FIELDS_46(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 47, CHECK_PACKED_FIELDS_47(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 48, CHECK_PACKED_FIELDS_48(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 49, CHECK_PACKED_FIELDS_49(fields), \
	__builtin_choose_expr(ARRAY_SIZE(fields) == 50, CHECK_PACKED_FIELDS_50(fields), \
	({ BUILD_BUG_ON_MSG(1, "CHECK_PACKED_FIELDS() must be regenerated to support array sizes larger than 50."); }) \
	))))))))))))))))))))))))))))))))))))))))))))))))))

#endif
