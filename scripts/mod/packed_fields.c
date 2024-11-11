// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024, Intel Corporation. */

/* Code to validate struct packed_field_[sm] data, and perform sanity checks
 * to ensure the packed field data is laid out correctly and fits into the
 * relevant buffer size.
 */

#include <fnmatch.h>
#include <hashtable.h>
#include <inttypes.h>
#include <stdint.h>
#include <xalloc.h>

#include "modpost.h"

typedef uint16_t	u16;
typedef uint8_t		u8;

#define BITS_PER_BYTE	8

/* Big exception to the "don't include kernel headers into userspace", which
 * even potentially has different endianness and word sizes, since we handle
 * those differences explicitly below
 */
#include "../../include/linux/packing_types.h"

#define max(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a > _b ? _a : _b; })

#define min(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a < _b ? _a : _b; })

struct packed_field_elem {
	uint64_t startbit;
	uint64_t endbit;
	uint64_t offset;
	uint64_t size;
};

enum field_type {
	UNKNOWN_SECTION,
	PACKED_FIELD_S,
	PACKED_FIELD_M,
};

enum element_order {
	FIRST_ELEMENT,
	SECOND_ELEMENT,
	ASCENDING_ORDER,
	DESCENDING_ORDER,
};

static size_t field_type_to_size(enum field_type type)
{
	switch (type) {
	case PACKED_FIELD_S:
		return sizeof(struct packed_field_s);
	case PACKED_FIELD_M:
		return sizeof(struct packed_field_m);
	default:
		error("attempted to get field size for unknown packed field type %u\n",
		      type);
		return 0;
	}
}

static void get_field_contents(const void *data, enum field_type type,
			       struct packed_field_elem *elem)
{
	switch (type) {
	case PACKED_FIELD_S: {
		const struct packed_field_s *data_field = data;

		elem->startbit = TO_NATIVE(data_field->startbit);
		elem->endbit = TO_NATIVE(data_field->endbit);
		elem->offset = TO_NATIVE(data_field->offset);
		elem->size = TO_NATIVE(data_field->size);
		return;
	}
	case PACKED_FIELD_M: {
		const struct packed_field_m *data_field = data;

		elem->startbit = TO_NATIVE(data_field->startbit);
		elem->endbit = TO_NATIVE(data_field->endbit);
		elem->offset = TO_NATIVE(data_field->offset);
		elem->size = TO_NATIVE(data_field->size);
		return;
	}
	default:
		error("attempted to get field contents for unknown packed field type %u\n",
		      type);
	}
}

void handle_packed_field_symbol(struct module *mod, struct elf_info *info,
				Elf_Sym *sym, const char *symname)
{
	unsigned int secindex = get_secindex(info, sym);
	struct packed_field_elem elem = {}, prev = {};
	enum element_order order = FIRST_ELEMENT;
	enum field_type type = UNKNOWN_SECTION;
	size_t field_size, count;
	const void *data, *ptr;
	const char *section;

	/* Skip symbols without a name */
	if (*symname == '\0')
		return;

	/* Skip symbols with invalid sections */
	if (secindex >= info->num_sections)
		return;

	section = sec_name(info, secindex);

	if (strcmp(section, ".rodata.packed_fields_s") == 0)
		type = PACKED_FIELD_S;
	else if (strcmp(section, ".rodata.packed_fields_m") == 0)
		type = PACKED_FIELD_M;

	/* Other sections don't relate to packed fields */
	if (type == UNKNOWN_SECTION)
		return;

	field_size = field_type_to_size(type);

	/* check that the data is a multiple of the size */
	if (sym->st_size % field_size != 0) {
		error("[%s.ko] \"%s\" has size %u which is not a multiple of the field size (%zu)\n",
		      mod->name, symname, sym->st_size, field_size);
		return;
	}

	data = sym_get_data(info, sym);

	for (ptr = data, count = 0;
	     ptr < data + sym->st_size;
	     ptr += field_size, count++, prev = elem) {
		get_field_contents(ptr, type, &elem);

		if (elem.size != 1 && elem.size != 2 &&
		    elem.size != 4 && elem.size != 8)
			error("[%s.ko] \"%s\" field %zu unpacked size (%" PRIu64 ") must be 1, 2, 4, or 8\n",
			      mod->name, symname, count, elem.size);

		if (elem.startbit < elem.endbit)
			error("[%s.ko] \"%s\" field %zu (%" PRIu64 "-%" PRIu64 "): start bit must be >= end bit\n",
			      mod->name, symname, count,
			      elem.startbit, elem.endbit);

		if (elem.startbit - elem.endbit + 1 > BITS_PER_BYTE * elem.size)
			error("[%s.ko] \"%s\" field %zu (%" PRIu64 "-%" PRIu64 ") has a width of %" PRIu64 " bits which does not fit into the unpacked structure field (%" PRIu64 " bytes)\n",
			      mod->name, symname, count,
			      elem.startbit, elem.endbit,
			      elem.startbit - elem.endbit + 1,
			      elem.size);

		if (order != FIRST_ELEMENT &&
		    max(elem.endbit, prev.endbit) <=
		    min(elem.startbit, prev.startbit))
			error("[%s.ko] \"%s\" field %zu (%" PRIu64 "-%" PRIu64 ") overlaps with previous field (%" PRIu64 "-%" PRIu64 ")\n",
			      mod->name, symname, count,
			      elem.startbit, elem.endbit,
			      prev.startbit, prev.endbit);

		switch (order) {
		case FIRST_ELEMENT:
			order = SECOND_ELEMENT;
			break;
		case SECOND_ELEMENT:
			order = prev.startbit < elem.startbit ?
				ASCENDING_ORDER : DESCENDING_ORDER;
			break;
		case ASCENDING_ORDER:
			if (prev.startbit >= elem.startbit ||
			    prev.endbit >= elem.endbit)
				error("[%s.ko] \"%s\" field %zu (%" PRIu64 "-%" PRIu64") not in ascending order with previous field (%" PRIu64 "-%" PRIu64 ")\n",
				      mod->name, symname, count,
				      elem.startbit, elem.endbit,
				      prev.startbit, prev.endbit);
			break;
		case DESCENDING_ORDER:
			if (prev.startbit <= elem.startbit ||
			    prev.endbit <= elem.endbit)
				error("[%s.ko] \"%s\" field %zu (%" PRIu64 "-%" PRIu64") not in descending order with previous field (%" PRIu64 "-%" PRIu64 ")\n",
				      mod->name, symname, count,
				      elem.startbit, elem.endbit,
				      prev.startbit, prev.endbit);
			break;
		default:
			break;
		}
	}
}
