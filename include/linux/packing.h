/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016-2018 NXP
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _LINUX_PACKING_H
#define _LINUX_PACKING_H

#include <linux/types.h>
#include <linux/bitops.h>

#define QUIRK_MSB_ON_THE_RIGHT	BIT(0)
#define QUIRK_LITTLE_ENDIAN	BIT(1)
#define QUIRK_LSW32_IS_FIRST	BIT(2)

enum packing_op {
	PACK,
	UNPACK,
};

int packing(void *pbuf, u64 *uval, int startbit, int endbit, size_t pbuflen,
	    enum packing_op op, u8 quirks);

int pack(void *pbuf, u64 uval, size_t startbit, size_t endbit, size_t pbuflen,
	 u8 quirks);

int unpack(const void *pbuf, u64 *uval, size_t startbit, size_t endbit,
	   size_t pbuflen, u8 quirks);

#define GEN_PACKED_FIELD_MEMBERS(__type) \
	__type startbit; \
	__type endbit; \
	__type offset; \
	__type size;

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

#if IS_ENABLED(CONFIG_PACKING_CHECK_FIELDS)

#define CHECK_PACKED_FIELD(field, pbuflen) \
	({ typeof(field) __f = (field); typeof(pbuflen) __len = (pbuflen); \
	BUILD_BUG_ON(__f.startbit < __f.endbit); \
	BUILD_BUG_ON(__f.startbit >= BITS_PER_BYTE * __len); \
	BUILD_BUG_ON(__f.startbit - __f.endbit >= BITS_PER_BYTE * __f.size); \
	BUILD_BUG_ON(__f.size != 1 && __f.size != 2 && __f.size != 4 && __f.size != 8); })

#define CHECK_PACKED_FIELD_OVERLAP(field1, field2) \
	({ typeof(field1) _f1 = (field1); typeof(field2) _f2 = (field2); \
	BUILD_BUG_ON(max(_f1.endbit, _f2.endbit) <=  min(_f1.startbit, _f2.startbit)); })

#include <generated/packing-checks.h>

#endif

void pack_fields_s(void *pbuf, size_t pbuflen, const void *ustruct,
		   const struct packed_field_s *fields, size_t num_fields,
		   u8 quirks);

void pack_fields_m(void *pbuf, size_t pbuflen, const void *ustruct,
		   const struct packed_field_m *fields, size_t num_fields,
		   u8 quirks);

void unpack_fields_s(const void *pbuf, size_t pbuflen, void *ustruct,
		     const struct packed_field_s *fields, size_t num_fields,
		     u8 quirks);

void unpack_fields_m(const void *pbuf, size_t pbuflen, void *ustruct,
		      const struct packed_field_m *fields, size_t num_fields,
		      u8 quirks);

#define pack_fields(pbuf, pbuflen, ustruct, fields, quirks) \
	_Generic((fields), \
		 const struct packed_field_s * : pack_fields_s, \
		 const struct packed_field_m * : pack_fields_m \
		)(pbuf, pbuflen, ustruct, fields, ARRAY_SIZE(fields), quirks)

#define unpack_fields(pbuf, pbuflen, ustruct, fields, quirks) \
	_Generic((fields), \
		 const struct packed_field_s * : unpack_fields_s, \
		 const struct packed_field_m * : unpack_fields_m \
		)(pbuf, pbuflen, ustruct, fields, ARRAY_SIZE(fields), quirks)

#endif
