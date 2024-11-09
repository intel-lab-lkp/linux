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
	__type size;

/* Small packed field. Use with bit offsets < 256, buffers < 32B and
 * unpacked structures < 256B.
 */
struct packed_field_s {
	GEN_PACKED_FIELD_MEMBERS(u8);
};

#define DECLARE_PACKED_FIELDS_S(name) \
	const struct packed_field_s name[] __section(".rodata.packed_fields_s")

/* Medium packed field. Use with bit offsets < 65536, buffers < 8KB and
 * unpacked structures < 64KB.
 */
struct packed_field_m {
	GEN_PACKED_FIELD_MEMBERS(u16);
};

#define DECLARE_PACKED_FIELDS_M(name) \
	const struct packed_field_m name[] __section(".rodata.packed_fields_m")

#define PACKED_FIELD(start, end, struct_name, struct_field) \
{ \
	(start), \
	(end), \
	offsetof(struct_name, struct_field), \
	sizeof_field(struct_name, struct_field), \
}

#endif
