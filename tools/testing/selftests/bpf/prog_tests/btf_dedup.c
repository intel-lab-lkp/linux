// SPDX-License-Identifier: GPL-2.0
#include <test_progs.h>
#include <bpf/btf.h>
#include "btf_helpers.h"

static void test_array_element_types(bool identical)
{
	struct btf *btf;

	btf = btf__new_empty();
	if (!ASSERT_OK_PTR(btf, "btf_new"))
		return;

	/*
	 * Comparing the first fields maps array [3] to [6]. The second
	 * fields reuse [3], requiring an identical-type check of [6]/[7].
	 * Place container [5] before its elements [8]/[9] so they still
	 * have distinct IDs when their definitions are compared.
	 */
	if (!ASSERT_EQ(btf__add_int(btf, "int", 4, BTF_INT_SIGNED), 1, "int") ||
	    !ASSERT_EQ(btf__add_struct(btf, "container", 8), 2, "container1") ||
	    !ASSERT_OK(btf__add_field(btf, "first", 3, 0, 0), "first1") ||
	    !ASSERT_OK(btf__add_field(btf, "second", 3, 32, 0), "second1") ||
	    !ASSERT_EQ(btf__add_array(btf, 1, 4, 1), 3, "array1") ||
	    !ASSERT_EQ(btf__add_struct(btf, "elem", 4), 4, "elem1") ||
	    !ASSERT_OK(btf__add_field(btf, "x", 1, 0, 0), "elem1_field") ||
	    !ASSERT_EQ(btf__add_struct(btf, "container", 8), 5, "container2") ||
	    !ASSERT_OK(btf__add_field(btf, "first", 6, 0, 0), "first2") ||
	    !ASSERT_OK(btf__add_field(btf, "second", 7, 32, 0), "second2") ||
	    !ASSERT_EQ(btf__add_array(btf, 1, 8, 1), 6, "array2") ||
	    !ASSERT_EQ(btf__add_array(btf, 1, 9, 1), 7, "array3") ||
	    !ASSERT_EQ(btf__add_struct(btf, "elem", 4), 8, "elem2") ||
	    !ASSERT_OK(btf__add_field(btf, "x", 1, 0, 0), "elem2_field") ||
	    !ASSERT_EQ(btf__add_struct(btf, "elem", 4), 9, "elem3") ||
	    !ASSERT_OK(btf__add_field(btf, identical ? "x" : "y", 1, 0, 0), "elem3_field"))
		goto out;

	if (!ASSERT_OK(btf__dedup(btf, NULL), "dedup"))
		goto out;

	/* Identical elements leave only the first four expected types. */
	btf_validate_raw(btf, identical ? 4 : 7, (const char *[]) {
		"[1] INT 'int' size=4 bits_offset=0 nr_bits=32 encoding=SIGNED",
		"[2] STRUCT 'container' size=8 vlen=2\n"
		"\t'first' type_id=3 bits_offset=0\n"
		"\t'second' type_id=3 bits_offset=32",
		"[3] ARRAY '(anon)' type_id=4 index_type_id=1 nr_elems=1",
		"[4] STRUCT 'elem' size=4 vlen=1\n"
		"\t'x' type_id=1 bits_offset=0",
		"[5] STRUCT 'container' size=8 vlen=2\n"
		"\t'first' type_id=3 bits_offset=0\n"
		"\t'second' type_id=6 bits_offset=32",
		"[6] ARRAY '(anon)' type_id=7 index_type_id=1 nr_elems=1",
		"[7] STRUCT 'elem' size=4 vlen=1\n"
		"\t'y' type_id=1 bits_offset=0",
	});

out:
	btf__free(btf);
}

void test_btf_dedup(void)
{
	if (test__start_subtest("array_different_element_types"))
		test_array_element_types(false);
	if (test__start_subtest("array_identical_element_types"))
		test_array_element_types(true);
}
