/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016-2018 NXP
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#ifndef _LINUX_PACKING_H
#define _LINUX_PACKING_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/packing_types.h>

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

#define pack_fields(pbuf, ustruct, fields, quirks) \
	({ \
		typeof(fields[0]) *__f = fields; \
		size_t pbuflen = sizeof(*pbuf); \
		size_t num_fields = ARRAY_SIZE(fields); \
		BUILD_BUG_ON(__f[0].startbit >= BITS_PER_BYTE * pbuflen); \
		BUILD_BUG_ON(__f[num_fields - 1].startbit >= BITS_PER_BYTE * pbuflen); \
		_Generic((fields), \
			 const struct packed_field_s * : pack_fields_s, \
			 const struct packed_field_m * : pack_fields_m \
			)(pbuf, pbuflen, ustruct, __f, num_fields, quirks); \
	})

#define unpack_fields(pbuf, ustruct, fields, quirks) \
	({ \
		typeof(fields[0]) *__f = fields; \
		size_t pbuflen = sizeof(*pbuf); \
		size_t num_fields = ARRAY_SIZE(fields); \
		BUILD_BUG_ON(__f[0].startbit >= BITS_PER_BYTE * pbuflen); \
		BUILD_BUG_ON(__f[num_fields - 1].startbit >= BITS_PER_BYTE * pbuflen); \
		_Generic((fields), \
			 const struct packed_field_s * : unpack_fields_s, \
			 const struct packed_field_m * : unpack_fields_m \
			)(pbuf, pbuflen, ustruct, __f, num_fields, quirks); \
	})

#endif
