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

struct field_symbol {
	struct hlist_node hnode;
	enum field_type type;
	size_t buffer_size;
	size_t data_size;
	void *data;
	struct module *mod;
	char *name;
};

static HASHTABLE_DEFINE(field_hashtable, 1U << 10);

static struct field_symbol *alloc_field(char *name, struct module *mod)
{
	struct field_symbol *f = xmalloc(sizeof(*f));

	memset(f, 0, sizeof(*f));
	f->mod = mod;
	f->name = name;

	return f;
}

static void hash_add_field(struct field_symbol *field)
{
	hash_add(field_hashtable, &field->hnode, tdb_hash(field->name));
}

static struct field_symbol *find_field(const char *name, struct module *mod)
{
	struct field_symbol *f;

	hash_for_each_possible(field_hashtable, f, hnode, tdb_hash(name)) {
		if (strcmp(f->name, name) == 0 && f->mod == mod)
			return f;
	}
	return NULL;
}

void handle_packed_field_symbol(struct module *mod, struct elf_info *info,
				Elf_Sym *sym, const char *symname)
{
	unsigned int secindex = get_secindex(info, sym);
	enum field_type type = UNKNOWN_SECTION;
	bool is_size_symbol = false;
	struct field_symbol *f;
	const char *section;
	char *name;

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

	name = xstrdup(symname);

	/* Extract original field name from the size symbol */
	if (!fnmatch("__*_buffer_sz", name, 0)) {
		name += strlen("__");
		name[strlen(name) - strlen("_buffer_sz")] = '\0';
		is_size_symbol = true;
	}

	f = find_field(name, mod);
	if (!f) {
		f = alloc_field(name, mod);
		f->type = type;
		hash_add_field(f);
	}

	if (f->type != type) {
		error("%s and %s have mismatched packed field sections\n",
		      f->name, symname);
		return;
	}

	if (is_size_symbol) {
		size_t *size_data = sym_get_data(info, sym);
		size_t size = TO_NATIVE(*size_data);

		if (f->buffer_size && f->buffer_size != size) {
			error("%s has buffer size %zu, but symbol %s says the size should be %zu\n",
			      f->name, f->buffer_size, symname, size);
		}

		f->buffer_size = size;
	} else {
		if (f->data)
			error("%s has multiple data symbols???\n",
			      f->name);

		f->data_size = sym->st_size;
		f->data = xmalloc(f->data_size);
		memcpy(f->data, sym_get_data(info, sym), f->data_size);
	}
}

enum element_order {
	FIRST_ELEMENT,
	SECOND_ELEMENT,
	ASCENDING_ORDER,
	DESCENDING_ORDER,
};

static void check_packed_field_array(const struct field_symbol *f)
{
	struct packed_field_elem previous_elem = {};
	size_t field_size = field_type_to_size(f->type);
	enum element_order order = FIRST_ELEMENT;
	void *data_ptr;
	int count;

	/* check that the data is a multiple of the size */
	if (f->data_size % field_size != 0) {
		error("symbol %s of module %s has size %zu which is not a multiple of the field size (%zu)\n",
		      f->name, f->mod->name, f->data_size, field_size);
		return;
	}

	data_ptr = f->data;
	count = 0;

	while (data_ptr < f->data + f->data_size) {
		struct packed_field_elem elem = {};

		get_field_contents(data_ptr, f->type, &elem);

		if (elem.startbit < elem.endbit)
			error("\"%s\" [%s.ko] element %u startbit (%" PRIu64 ") must be larger than endbit (%" PRIu64 ")\n",
			      f->name, f->mod->name, count, elem.startbit,
			      elem.endbit);

		if (elem.startbit >= BITS_PER_BYTE * f->buffer_size)
			error("\"%s\" [%s.ko] element %u startbit (%" PRIu64 ") puts field outsize of the packed buffer size (%" PRIu64 ")\n",
			      f->name, f->mod->name, count, elem.startbit,
			      f->buffer_size);

		if (elem.startbit - elem.endbit >= BITS_PER_BYTE * elem.size)
			error("\"%s\" [%s.ko] element %u startbit (%" PRIu64 ") and endbit (%" PRIu64 ") indicate a field of width (%" PRIu64 ") which does not fit into the field size (%" PRIu64 ")\n",
			      f->name, f->mod->name, count, elem.startbit,
			      elem.endbit, elem.startbit - elem.endbit,
			      elem.size);

		if (elem.size != 1 && elem.size != 2 && elem.size != 4 && elem.size != 8)
			error("\"%s\" [%s.ko] element %u size (%" PRIu64 ") must be 1, 2, 4, or 8\n",
			      f->name, f->mod->name, count, elem.size);

		switch (order) {
		case FIRST_ELEMENT:
			order = SECOND_ELEMENT;
			break;
		case SECOND_ELEMENT:
			order = previous_elem.startbit < elem.startbit ?
				ASCENDING_ORDER : DESCENDING_ORDER;
			break;
		default:
			break;
		}

		switch (order) {
		case ASCENDING_ORDER:
			if (previous_elem.startbit >= elem.startbit)
				error("\"%s\" [%s.ko] element %u startbit (%" PRIu64 ") expected to be arranged in ascending order, but previous element startbit is %" PRIu64 "\n",
				      f->name, f->mod->name, count,
				      elem.startbit, previous_elem.startbit);
			if (previous_elem.endbit >= elem.endbit)
				error("\"%s\" [%s.ko] element %u endbit (%" PRIu64 ") expected to be arranged in ascending order, but previous element endbit is %" PRIu64 "\n",
				      f->name, f->mod->name, count, elem.endbit,
				      previous_elem.endbit);

			break;
		case DESCENDING_ORDER:
			if (previous_elem.startbit <= elem.startbit)
				error("\"%s\" [%s.ko] element %u startbit (%" PRIu64 ") expected to be arranged in descending order, but previous element startbit is %" PRIu64 "\n",
				      f->name, f->mod->name, count,
				      elem.startbit, previous_elem.startbit);
			if (previous_elem.endbit <= elem.endbit)
				error("\"%s\" [%s.ko] element %u endbit (%" PRIu64 ") expected to be arranged in descending order, but previous element endbit is %" PRIu64 "\n",
				      f->name, f->mod->name, count,
				      elem.endbit, previous_elem.endbit);
			break;
		default:
			break;
		}

		previous_elem = elem;
		data_ptr += field_size;
		count++;
	}
}

void check_packed_field_symbols(void)
{
	struct field_symbol *f;

	hash_for_each(field_hashtable, f, hnode)
		check_packed_field_array(f);
}
