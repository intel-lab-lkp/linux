// SPDX-License-Identifier: GPL-2.0
#include <linux/btf.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "trace_btf.h"

/*
 * Find a function proto type by name, and return the btf_type with its btf
 * in *@btf_p. Return NULL if not found.
 * Note that caller has to call btf_put(*@btf_p) after using the btf_type.
 */
const struct btf_type *btf_find_func_proto(const char *func_name, struct btf **btf_p)
{
	const struct btf_type *t;
	s32 id;

	id = bpf_find_btf_id(func_name, BTF_KIND_FUNC, btf_p);
	if (id < 0)
		return NULL;

	/* Get BTF_KIND_FUNC type */
	t = btf_type_by_id(*btf_p, id);
	if (!t || !btf_type_is_func(t))
		goto err;

	/* The type of BTF_KIND_FUNC is BTF_KIND_FUNC_PROTO */
	t = btf_type_by_id(*btf_p, t->type);
	if (!t || !btf_type_is_func_proto(t))
		goto err;

	return t;
err:
	btf_put(*btf_p);
	return NULL;
}

/*
 * Get function parameter with the number of parameters.
 * This can return NULL if the function has no parameters.
 * It can return -EINVAL if the @func_proto is not a function proto type.
 */
const struct btf_param *btf_get_func_param(const struct btf_type *func_proto, s32 *nr)
{
	if (!btf_type_is_func_proto(func_proto))
		return ERR_PTR(-EINVAL);

	*nr = btf_type_vlen(func_proto);
	if (*nr > 0)
		return (const struct btf_param *)(func_proto + 1);
	else
		return NULL;
}

#define BTF_ANON_STACK_MAX	16

struct btf_anon_stack {
	u32 tid;
	u32 offset;
};

/*
 * Find a member of data structure/union by name and return it.
 * Return NULL if not found, or -EINVAL if parameter is invalid.
 * If the member is an member of anonymous union/structure, the offset
 * of that anonymous union/structure is stored into @anon_offset. Caller
 * can calculate the correct offset from the root data structure by
 * adding anon_offset to the member's offset.
 */
const struct btf_member *btf_find_struct_member(struct btf *btf,
						const struct btf_type *type,
						const char *member_name,
						u32 *anon_offset)
{
	struct btf_anon_stack *anon_stack;
	const struct btf_member *member;
	u32 tid, cur_offset = 0;
	const char *name;
	int i, top = 0;

	anon_stack = kzalloc_objs(*anon_stack, BTF_ANON_STACK_MAX);
	if (!anon_stack)
		return ERR_PTR(-ENOMEM);

retry:
	if (!btf_type_is_struct(type)) {
		member = ERR_PTR(-EINVAL);
		goto out;
	}

	for_each_member(i, type, member) {
		if (!member->name_off) {
			/* Anonymous union/struct: push it for later use */
			if (btf_type_skip_modifiers(btf, member->type, &tid) &&
			    top < BTF_ANON_STACK_MAX) {
				anon_stack[top].tid = tid;
				anon_stack[top++].offset =
					cur_offset + member->offset;
			}
		} else {
			name = btf_name_by_offset(btf, member->name_off);
			if (name && !strcmp(member_name, name)) {
				if (anon_offset)
					*anon_offset = cur_offset;
				goto out;
			}
		}
	}
	if (top > 0) {
		/* Pop from the anonymous stack and retry */
		tid = anon_stack[--top].tid;
		cur_offset = anon_stack[top].offset;
		type = btf_type_by_id(btf, tid);
		goto retry;
	}
	member = NULL;

out:
	kfree(anon_stack);
	return member;
}

#define BITS_ROUNDDOWN_BYTES(bits) ((bits) >> 3)

static int find_member(const char *ptr, struct btf *btf,
		       const struct btf_type **type, int level)
{
	const struct btf_member *member;
	const struct btf_type *t = *type;
	int i;

	/* Max of 3 depth of anonymous structures */
	if (level > 3)
		return -E2BIG;

	for_each_member(i, t, member) {
		const char *tname = btf_name_by_offset(btf, member->name_off);

		if (strcmp(ptr, tname) == 0) {
			int offset = __btf_member_bit_offset(t, member);
			*type = btf_type_by_id(btf, member->type);
			return BITS_ROUNDDOWN_BYTES(offset);
		}

		/* Handle anonymous structures */
		if (strlen(tname))
			continue;

		*type = btf_type_by_id(btf, member->type);
		if (btf_type_is_struct(*type)) {
			int offset = find_member(ptr, btf, type, level + 1);

			if (offset < 0)
				continue;

			return offset + BITS_ROUNDDOWN_BYTES(member->offset);
		}
	}

	return -ENOENT;
}

/**
 * btf_find_offset - Find an offset of a member for a structure
 * @arg: A structure name followed by one or more members
 * @offset_p: A pointer to where to store the offset
 *
 * Will parse @arg with the expected format of: struct.member[[.member]..]
 * It is delimited by '.'. The first item must be a structure type.
 * The next are its members. If the member is also of a structure type it
 * another member may follow ".member".
 *
 * Note, @arg is modified but will be put back to what it was on return.
 *
 * Returns: 0 on success and -EINVAL if no '.' is present
 *    or -ENXIO if the structure or member is not found.
 *    Returns -EINVAL if BTF is not defined.
 *  On success, @offset_p will contain the offset of the member specified
 *    by @arg.
 */
int btf_find_offset(char *arg, long *offset_p)
{
	const struct btf_type *t;
	struct btf *btf;
	long offset = 0;
	char *ptr;
	int ret;
	s32 id;

	ptr = strchr(arg, '.');
	if (!ptr)
		return -EINVAL;

	*ptr = '\0';

	ret = -ENXIO;
	id = bpf_find_btf_id(arg, BTF_KIND_STRUCT, &btf);
	if (id < 0)
		goto error;

	/* Get BTF_KIND_FUNC type */
	t = btf_type_by_id(btf, id);

	/* May allow more than one member, as long as they are structures */
	do {
		ret = -ENXIO;
		if (!t || !btf_type_is_struct(t))
			goto error;

		*ptr++ = '.';
		arg = ptr;
		ptr = strchr(ptr, '.');
		if (ptr)
			*ptr = '\0';

		ret = find_member(arg, btf, &t, 0);
		if (ret < 0)
			goto error;

		offset += ret;

	} while (ptr);

	btf_put(btf);
	*offset_p = offset;
	return 0;

error:
	btf_put(btf);
	if (ptr)
		*ptr = '.';
	return ret;
}
