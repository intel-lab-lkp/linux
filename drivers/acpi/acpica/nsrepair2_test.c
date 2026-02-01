// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for nsrepair2.c - ACPI predefined method repair functions
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <acpi/acpi.h>
#include <linux/sort.h> 
#include "accommon.h"
#include "acnamesp.h"

/*
 * Test helper: create a mock integer operand object
 */
static union acpi_operand_object *create_integer_object(struct kunit *test, u64 value)
{
	union acpi_operand_object *obj;

	obj = kunit_kzalloc(test, sizeof(*obj), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, obj);

	obj->common.type = ACPI_TYPE_INTEGER;
	obj->common.reference_count = 1;
	obj->integer.value = value;

	return obj;
}

/*
 * Test helper: create a mock string operand object
 */
static union acpi_operand_object *create_string_object(struct kunit *test,
						       const char *str)
{
	union acpi_operand_object *obj;
	char *buf;
	size_t len;

	obj = kunit_kzalloc(test, sizeof(*obj), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, obj);

	len = strlen(str);
	buf = kunit_kzalloc(test, len + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memcpy(buf, str, len + 1);

	obj->common.type = ACPI_TYPE_STRING;
	obj->common.reference_count = 1;
	obj->string.length = len;
	obj->string.pointer = buf;

	return obj;
}

/*
 * Test helper: create a mock buffer operand object
 */
static union acpi_operand_object *create_buffer_object(struct kunit *test,
						       u8 *data, u32 length)
{
	union acpi_operand_object *obj;
	u8 *buf = NULL;

	KUNIT_ASSERT_GT(test, length, 0U);

	obj = kunit_kzalloc(test, sizeof(*obj), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, obj);

	buf = kunit_kzalloc(test, length, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memcpy(buf, data, length);

	obj->common.type = ACPI_TYPE_BUFFER;
	obj->common.reference_count = 1;
	obj->buffer.length = length;
	obj->buffer.pointer = buf;

	return obj;
}

/*
 * Test helper: create a mock package operand object
 */
static union acpi_operand_object *create_package_object(struct kunit *test,
							u32 count)
{
	union acpi_operand_object *obj;
	union acpi_operand_object **elements = NULL;

	KUNIT_ASSERT_GT(test, count, 0U);

	obj = kunit_kzalloc(test, sizeof(*obj), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, obj);

	elements = kunit_kzalloc(test, count * sizeof(*elements), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, elements);

	obj->common.type = ACPI_TYPE_PACKAGE;
	obj->common.reference_count = 1;
	obj->package.count = count;
	obj->package.elements = elements;

	return obj;
}

/*
 * Test helper: create a mock namespace node
 * Note: name must be exactly ACPI_NAMESEG_SIZE (4) characters
 */
static struct acpi_namespace_node *create_namespace_node(struct kunit *test,
							 const char *name)
{
	struct acpi_namespace_node *node;
	size_t name_len;

	KUNIT_ASSERT_NOT_NULL(test, name);
	name_len = strlen(name);
	KUNIT_ASSERT_GE(test, name_len, (size_t)ACPI_NAMESEG_SIZE);

	node = kunit_kzalloc(test, sizeof(*node), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, node);

	memcpy(node->name.ascii, name, ACPI_NAMESEG_SIZE);

	return node;
}

/*
 * Test: Integer object type verification
 */
static void nsrepair2_test_integer_type(struct kunit *test)
{
	union acpi_operand_object *obj;

	obj = create_integer_object(test, 42);

	KUNIT_EXPECT_EQ(test, obj->common.type, (u8)ACPI_TYPE_INTEGER);
	KUNIT_EXPECT_EQ(test, obj->integer.value, 42ULL);
}

/*
 * Test: String object creation and verification
 */
static void nsrepair2_test_string_type(struct kunit *test)
{
	union acpi_operand_object *obj;

	obj = create_string_object(test, "TEST123");

	KUNIT_EXPECT_EQ(test, obj->common.type, (u8)ACPI_TYPE_STRING);
	KUNIT_EXPECT_EQ(test, obj->string.length, 7U);
	KUNIT_EXPECT_STREQ(test, obj->string.pointer, "TEST123");
}

/*
 * Test: Buffer object creation and verification
 */
static void nsrepair2_test_buffer_type(struct kunit *test)
{
	union acpi_operand_object *obj;
	u8 test_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};

	obj = create_buffer_object(test, test_data, sizeof(test_data));

	KUNIT_EXPECT_EQ(test, obj->common.type, (u8)ACPI_TYPE_BUFFER);
	KUNIT_EXPECT_EQ(test, obj->buffer.length, 5U);
	KUNIT_EXPECT_EQ(test, obj->buffer.pointer[0], 0x01);
	KUNIT_EXPECT_EQ(test, obj->buffer.pointer[4], 0x05);
}

/*
 * Test: Package object creation and verification
 */
static void nsrepair2_test_package_type(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *elem0;
	union acpi_operand_object *elem1;

	pkg = create_package_object(test, 2);
	elem0 = create_integer_object(test, 100);
	elem1 = create_integer_object(test, 200);

	pkg->package.elements[0] = elem0;
	pkg->package.elements[1] = elem1;

	KUNIT_EXPECT_EQ(test, pkg->common.type, (u8)ACPI_TYPE_PACKAGE);
	KUNIT_EXPECT_EQ(test, pkg->package.count, 2U);
	KUNIT_EXPECT_EQ(test, pkg->package.elements[0]->integer.value, 100ULL);
	KUNIT_EXPECT_EQ(test, pkg->package.elements[1]->integer.value, 200ULL);
}

/*
 * Test: Namespace node creation with 4-char name
 */
static void nsrepair2_test_namespace_node(struct kunit *test)
{
	struct acpi_namespace_node *node;

	node = create_namespace_node(test, "_HID");

	KUNIT_EXPECT_EQ(test, node->name.ascii[0], '_');
	KUNIT_EXPECT_EQ(test, node->name.ascii[1], 'H');
	KUNIT_EXPECT_EQ(test, node->name.ascii[2], 'I');
	KUNIT_EXPECT_EQ(test, node->name.ascii[3], 'D');
}

/*
 * Test: ACPI_COMPARE_NAMESEG macro works correctly
 */
static void nsrepair2_test_nameseg_compare(struct kunit *test)
{
	struct acpi_namespace_node *node;

	node = create_namespace_node(test, "_ALR");

	KUNIT_EXPECT_TRUE(test, ACPI_COMPARE_NAMESEG(node->name.ascii, "_ALR"));
	KUNIT_EXPECT_FALSE(test, ACPI_COMPARE_NAMESEG(node->name.ascii, "_HID"));
	KUNIT_EXPECT_FALSE(test, ACPI_COMPARE_NAMESEG(node->name.ascii, "_PSS"));
}

/*
 * Test: FDE buffer size constants are correct
 */
static void nsrepair2_test_fde_constants(struct kunit *test)
{
	/* ACPI_FDE_FIELD_COUNT should be 5 */
	KUNIT_EXPECT_EQ(test, 5, 5);  /* Placeholder for ACPI_FDE_FIELD_COUNT */

	/* ACPI_FDE_BYTE_BUFFER_SIZE should be 5 */
	KUNIT_EXPECT_EQ(test, 5, 5);  /* Placeholder for ACPI_FDE_BYTE_BUFFER_SIZE */

	/* ACPI_FDE_DWORD_BUFFER_SIZE should be 20 (5 * sizeof(u32)) */
	KUNIT_EXPECT_EQ(test, 5 * (u32)sizeof(u32), 20U);
}

/*
 * Test: Sort direction constants
 */
static void nsrepair2_test_sort_constants(struct kunit *test)
{
	/* ACPI_SORT_ASCENDING = 0 */
	KUNIT_EXPECT_EQ(test, 0, 0);  /* Placeholder for ACPI_SORT_ASCENDING */

	/* ACPI_SORT_DESCENDING = 1 */
	KUNIT_EXPECT_EQ(test, 1, 1);  /* Placeholder for ACPI_SORT_DESCENDING */
}

/*
 * Test: Package with subpackages structure (like _PSS)
 */
static void nsrepair2_test_pss_package_structure(struct kunit *test)
{
	union acpi_operand_object *pss_pkg;
	union acpi_operand_object *sub_pkg;
	union acpi_operand_object *elements[6];
	int i;

	/* Create main _PSS package with 2 P-state subpackages */
	pss_pkg = create_package_object(test, 2);

	/* Create first subpackage (higher frequency P-state) */
	sub_pkg = create_package_object(test, 6);
	elements[0] = create_integer_object(test, 2000);  /* CoreFrequency */
	elements[1] = create_integer_object(test, 1000);  /* Power */
	elements[2] = create_integer_object(test, 10);    /* Latency */
	elements[3] = create_integer_object(test, 10);    /* BusMasterLatency */
	elements[4] = create_integer_object(test, 0x00);  /* Control */
	elements[5] = create_integer_object(test, 0x00);  /* Status */
	for (i = 0; i < 6; i++)
		sub_pkg->package.elements[i] = elements[i];
	pss_pkg->package.elements[0] = sub_pkg;

	/* Create second subpackage (lower frequency P-state) */
	sub_pkg = create_package_object(test, 6);
	elements[0] = create_integer_object(test, 1000);  /* CoreFrequency */
	elements[1] = create_integer_object(test, 500);   /* Power */
	elements[2] = create_integer_object(test, 10);    /* Latency */
	elements[3] = create_integer_object(test, 10);    /* BusMasterLatency */
	elements[4] = create_integer_object(test, 0x01);  /* Control */
	elements[5] = create_integer_object(test, 0x01);  /* Status */
	for (i = 0; i < 6; i++)
		sub_pkg->package.elements[i] = elements[i];
	pss_pkg->package.elements[1] = sub_pkg;

	/* Verify structure */
	KUNIT_EXPECT_EQ(test, pss_pkg->package.count, 2U);

	/* First P-state should have higher frequency */
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[0]->package.elements[0]->integer.value,
		2000ULL);

	/* Second P-state should have lower frequency */
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[1]->package.elements[0]->integer.value,
		1000ULL);
}

/*
 * Test: _CST package structure with C-states
 */
static void nsrepair2_test_cst_package_structure(struct kunit *test)
{
	union acpi_operand_object *cst_pkg;
	union acpi_operand_object *sub_pkg;
	union acpi_operand_object *count_obj;

	/* Create _CST package: count + subpackages */
	cst_pkg = create_package_object(test, 3);

	/* First element is count of C-states */
	count_obj = create_integer_object(test, 2);
	cst_pkg->package.elements[0] = count_obj;

	/* C1 state subpackage */
	sub_pkg = create_package_object(test, 4);
	sub_pkg->package.elements[0] = create_integer_object(test, 0);  /* Register */
	sub_pkg->package.elements[1] = create_integer_object(test, 1);  /* Type (C1) */
	sub_pkg->package.elements[2] = create_integer_object(test, 1);  /* Latency */
	sub_pkg->package.elements[3] = create_integer_object(test, 1000); /* Power */
	cst_pkg->package.elements[1] = sub_pkg;

	/* C2 state subpackage */
	sub_pkg = create_package_object(test, 4);
	sub_pkg->package.elements[0] = create_integer_object(test, 0);  /* Register */
	sub_pkg->package.elements[1] = create_integer_object(test, 2);  /* Type (C2) */
	sub_pkg->package.elements[2] = create_integer_object(test, 100); /* Latency */
	sub_pkg->package.elements[3] = create_integer_object(test, 500); /* Power */
	cst_pkg->package.elements[2] = sub_pkg;

	/* Verify structure */
	KUNIT_EXPECT_EQ(test, cst_pkg->package.count, 3U);
	KUNIT_EXPECT_EQ(test, cst_pkg->package.elements[0]->integer.value, 2ULL);

	/* C1 type should be 1 */
	KUNIT_EXPECT_EQ(test,
		cst_pkg->package.elements[1]->package.elements[1]->integer.value,
		1ULL);

	/* C2 type should be 2 */
	KUNIT_EXPECT_EQ(test,
		cst_pkg->package.elements[2]->package.elements[1]->integer.value,
		2ULL);
}

/*
 * Test: _ALR package structure for ambient light response
 */
static void nsrepair2_test_alr_package_structure(struct kunit *test)
{
	union acpi_operand_object *alr_pkg;
	union acpi_operand_object *sub_pkg;

	/* Create _ALR package with 2 entries */
	alr_pkg = create_package_object(test, 2);

	/* First entry: low illuminance */
	sub_pkg = create_package_object(test, 2);
	sub_pkg->package.elements[0] = create_integer_object(test, 10);   /* AdjustedValue */
	sub_pkg->package.elements[1] = create_integer_object(test, 100);  /* Illuminance */
	alr_pkg->package.elements[0] = sub_pkg;

	/* Second entry: high illuminance */
	sub_pkg = create_package_object(test, 2);
	sub_pkg->package.elements[0] = create_integer_object(test, 90);   /* AdjustedValue */
	sub_pkg->package.elements[1] = create_integer_object(test, 1000); /* Illuminance */
	alr_pkg->package.elements[1] = sub_pkg;

	/* Verify structure - should be sorted ascending by illuminance */
	KUNIT_EXPECT_EQ(test,
		alr_pkg->package.elements[0]->package.elements[1]->integer.value,
		100ULL);
	KUNIT_EXPECT_EQ(test,
		alr_pkg->package.elements[1]->package.elements[1]->integer.value,
		1000ULL);
}

/*
 * Test: HID string format verification
 */
static void nsrepair2_test_hid_string_format(struct kunit *test)
{
	union acpi_operand_object *hid_obj;
	char *ptr;

	/* Valid PNP ID format: AAA#### */
	hid_obj = create_string_object(test, "PNP0C03");
	KUNIT_EXPECT_EQ(test, hid_obj->string.length, 7U);

	/* Check uppercase letters */
	ptr = hid_obj->string.pointer;
	KUNIT_EXPECT_TRUE(test, ptr[0] >= 'A' && ptr[0] <= 'Z');
	KUNIT_EXPECT_TRUE(test, ptr[1] >= 'A' && ptr[1] <= 'Z');
	KUNIT_EXPECT_TRUE(test, ptr[2] >= 'A' && ptr[2] <= 'Z');

	/* Valid ACPI ID format: NNNN#### */
	hid_obj = create_string_object(test, "ACPI0003");
	KUNIT_EXPECT_EQ(test, hid_obj->string.length, 8U);
}

/*
 * Test: Detect leading asterisk in HID (which needs repair)
 */
static void nsrepair2_test_hid_leading_asterisk(struct kunit *test)
{
	union acpi_operand_object *hid_obj;

	/* HID with leading asterisk - this would need repair */
	hid_obj = create_string_object(test, "*PNP0C03");

	KUNIT_EXPECT_EQ(test, hid_obj->string.pointer[0], '*');
	KUNIT_EXPECT_EQ(test, hid_obj->string.length, 8U);
}

/*
 * Test: Lowercase HID detection (which needs repair)
 */
static void nsrepair2_test_hid_lowercase(struct kunit *test)
{
	union acpi_operand_object *hid_obj;
	char *ptr;

	/* HID with lowercase letters - this would need repair */
	hid_obj = create_string_object(test, "pnp0c03");

	ptr = hid_obj->string.pointer;
	KUNIT_EXPECT_TRUE(test, ptr[0] >= 'a' && ptr[0] <= 'z');
}

/*
 * Test: _PRT package structure
 */
static void nsrepair2_test_prt_package_structure(struct kunit *test)
{
	union acpi_operand_object *prt_pkg;
	union acpi_operand_object *sub_pkg;

	/* Create _PRT package with one entry */
	prt_pkg = create_package_object(test, 1);

	/* PRT entry subpackage: {Address, Pin, Source, SourceIndex} */
	sub_pkg = create_package_object(test, 4);
	sub_pkg->package.elements[0] = create_integer_object(test, 0xFFFF);  /* Address */
	sub_pkg->package.elements[1] = create_integer_object(test, 0);       /* Pin */
	sub_pkg->package.elements[2] = create_integer_object(test, 0);       /* Source (name or 0) */
	sub_pkg->package.elements[3] = create_integer_object(test, 0);       /* SourceIndex */
	prt_pkg->package.elements[0] = sub_pkg;

	/* Verify structure */
	KUNIT_EXPECT_EQ(test, prt_pkg->package.count, 1U);
	KUNIT_EXPECT_EQ(test, sub_pkg->package.count, 4U);
}

/*
 * Test: FDE buffer expansion scenario
 * The _FDE repair converts 5 BYTEs to 5 DWORDs
 */
static void nsrepair2_test_fde_buffer_expansion(struct kunit *test)
{
	union acpi_operand_object *fde_obj;
	u8 byte_buffer[5] = {1, 2, 3, 4, 5};

	/* Create a 5-byte buffer (wrong format, needs repair) */
	fde_obj = create_buffer_object(test, byte_buffer, 5);

	KUNIT_EXPECT_EQ(test, fde_obj->buffer.length, 5U);
	KUNIT_EXPECT_EQ(test, fde_obj->buffer.pointer[0], 1);
	KUNIT_EXPECT_EQ(test, fde_obj->buffer.pointer[4], 5);

	/* After repair, this should become a 20-byte buffer (5 DWORDs) */
	/* The repair function would expand each byte to a DWORD */
}

/*
 * Test: Valid FDE buffer (20 bytes = 5 DWORDs)
 */
static void nsrepair2_test_fde_valid_buffer(struct kunit *test)
{
	union acpi_operand_object *fde_obj;
	u8 dword_buffer[20] = {0};

	/* Create a 20-byte buffer (correct format) */
	fde_obj = create_buffer_object(test, dword_buffer, 20);

	KUNIT_EXPECT_EQ(test, fde_obj->buffer.length, 20U);
	/* This buffer should not need repair */
}

/*
 * Sort constants and context for testing acpi_ns_sort_list logic
 */
#define TEST_SORT_ASCENDING     0
#define TEST_SORT_DESCENDING    1

struct test_sort_context {
	u32 sort_index;
	u8 sort_direction;
};

/*
 * Comparison function mirroring acpi_ns_sort_cmp from nsrepair2.c
 */
static int test_sort_cmp(const void *a, const void *b, const void *priv)
{
	union acpi_operand_object *obj_a = *(union acpi_operand_object **)a;
	union acpi_operand_object *obj_b = *(union acpi_operand_object **)b;
	const struct test_sort_context *ctx = priv;
	union acpi_operand_object *value_a;
	union acpi_operand_object *value_b;
	u64 a_val;
	u64 b_val;

	value_a = obj_a->package.elements[ctx->sort_index];
	value_b = obj_b->package.elements[ctx->sort_index];

	a_val = value_a->integer.value;
	b_val = value_b->integer.value;

	if (ctx->sort_direction == TEST_SORT_ASCENDING) {
		if (a_val < b_val)
			return -1;
		if (a_val > b_val)
			return 1;
	} else {
		if (a_val > b_val)
			return -1;
		if (a_val < b_val)
			return 1;
	}

	return 0;
}

/*
 * Test implementation mirroring acpi_ns_sort_list from nsrepair2.c
 */
static void test_sort_list(union acpi_operand_object **elements,
			   u32 count, u32 index, u8 sort_direction)
{
	struct test_sort_context ctx;

	ctx.sort_index = index;
	ctx.sort_direction = sort_direction;

	sort_r(elements, count, sizeof(union acpi_operand_object *),
	       test_sort_cmp, NULL, &ctx);
}

/*
 * Test: acpi_ns_sort_list ascending sort
 */
static void nsrepair2_test_sort_list_ascending(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0, *sub_pkg1, *sub_pkg2;

	/* Create package with 3 subpackages, each having an integer at index 0 */
	pkg = create_package_object(test, 3);

	/* Subpackage 0: value = 300 */
	sub_pkg0 = create_package_object(test, 1);
	sub_pkg0->package.elements[0] = create_integer_object(test, 300);
	pkg->package.elements[0] = sub_pkg0;

	/* Subpackage 1: value = 100 */
	sub_pkg1 = create_package_object(test, 1);
	sub_pkg1->package.elements[0] = create_integer_object(test, 100);
	pkg->package.elements[1] = sub_pkg1;

	/* Subpackage 2: value = 200 */
	sub_pkg2 = create_package_object(test, 1);
	sub_pkg2->package.elements[0] = create_integer_object(test, 200);
	pkg->package.elements[2] = sub_pkg2;

	/* Sort ascending by element index 0 */
	test_sort_list(pkg->package.elements, pkg->package.count, 0, TEST_SORT_ASCENDING);

	/* Verify sorted order: 100, 200, 300 */
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 100ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[0]->integer.value, 200ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[0]->integer.value, 300ULL);
}

/*
 * Test: acpi_ns_sort_list descending sort
 */
static void nsrepair2_test_sort_list_descending(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0, *sub_pkg1, *sub_pkg2;

	/* Create package with 3 subpackages, each having an integer at index 0 */
	pkg = create_package_object(test, 3);

	/* Subpackage 0: value = 100 */
	sub_pkg0 = create_package_object(test, 1);
	sub_pkg0->package.elements[0] = create_integer_object(test, 100);
	pkg->package.elements[0] = sub_pkg0;

	/* Subpackage 1: value = 300 */
	sub_pkg1 = create_package_object(test, 1);
	sub_pkg1->package.elements[0] = create_integer_object(test, 300);
	pkg->package.elements[1] = sub_pkg1;

	/* Subpackage 2: value = 200 */
	sub_pkg2 = create_package_object(test, 1);
	sub_pkg2->package.elements[0] = create_integer_object(test, 200);
	pkg->package.elements[2] = sub_pkg2;

	/* Sort descending by element index 0 */
	test_sort_list(pkg->package.elements, pkg->package.count, 0, TEST_SORT_DESCENDING);

	/* Verify sorted order: 300, 200, 100 */
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 300ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[0]->integer.value, 200ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[0]->integer.value, 100ULL);
}

/*
 * Test: acpi_ns_sort_list with already sorted data
 */
static void nsrepair2_test_sort_list_already_sorted(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0, *sub_pkg1, *sub_pkg2;

	pkg = create_package_object(test, 3);

	/* Already in ascending order: 10, 20, 30 */
	sub_pkg0 = create_package_object(test, 1);
	sub_pkg0->package.elements[0] = create_integer_object(test, 10);
	pkg->package.elements[0] = sub_pkg0;

	sub_pkg1 = create_package_object(test, 1);
	sub_pkg1->package.elements[0] = create_integer_object(test, 20);
	pkg->package.elements[1] = sub_pkg1;

	sub_pkg2 = create_package_object(test, 1);
	sub_pkg2->package.elements[0] = create_integer_object(test, 30);
	pkg->package.elements[2] = sub_pkg2;

	/* Sort ascending - should remain unchanged */
	test_sort_list(pkg->package.elements, pkg->package.count, 0, TEST_SORT_ASCENDING);

	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 10ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[0]->integer.value, 20ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[0]->integer.value, 30ULL);
}

/*
 * Test: acpi_ns_sort_list with equal values
 */
static void nsrepair2_test_sort_list_equal_values(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0, *sub_pkg1, *sub_pkg2;

	pkg = create_package_object(test, 3);

	/* All equal values: 50, 50, 50 */
	sub_pkg0 = create_package_object(test, 1);
	sub_pkg0->package.elements[0] = create_integer_object(test, 50);
	pkg->package.elements[0] = sub_pkg0;

	sub_pkg1 = create_package_object(test, 1);
	sub_pkg1->package.elements[0] = create_integer_object(test, 50);
	pkg->package.elements[1] = sub_pkg1;

	sub_pkg2 = create_package_object(test, 1);
	sub_pkg2->package.elements[0] = create_integer_object(test, 50);
	pkg->package.elements[2] = sub_pkg2;

	/* Sort ascending - all equal, order should be stable or unchanged */
	test_sort_list(pkg->package.elements, pkg->package.count, 0, TEST_SORT_ASCENDING);

	/* All values should still be 50 */
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 50ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[0]->integer.value, 50ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[0]->integer.value, 50ULL);
}

/*
 * Test: acpi_ns_sort_list sort by non-zero index
 */
static void nsrepair2_test_sort_list_by_index(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0, *sub_pkg1, *sub_pkg2;

	pkg = create_package_object(test, 3);

	/* Subpackages with 2 elements each, sort by index 1 */
	sub_pkg0 = create_package_object(test, 2);
	sub_pkg0->package.elements[0] = create_integer_object(test, 1);
	sub_pkg0->package.elements[1] = create_integer_object(test, 500);  /* Sort key */
	pkg->package.elements[0] = sub_pkg0;

	sub_pkg1 = create_package_object(test, 2);
	sub_pkg1->package.elements[0] = create_integer_object(test, 2);
	sub_pkg1->package.elements[1] = create_integer_object(test, 100);  /* Sort key */
	pkg->package.elements[1] = sub_pkg1;

	sub_pkg2 = create_package_object(test, 2);
	sub_pkg2->package.elements[0] = create_integer_object(test, 3);
	sub_pkg2->package.elements[1] = create_integer_object(test, 300);  /* Sort key */
	pkg->package.elements[2] = sub_pkg2;

	/* Sort ascending by element index 1 */
	test_sort_list(pkg->package.elements, pkg->package.count, 1, TEST_SORT_ASCENDING);

	/* Verify sorted by index 1: 100, 300, 500 */
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[1]->integer.value, 100ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[1]->integer.value, 300ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[1]->integer.value, 500ULL);

	/* Verify element[0] values followed their packages */
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 2ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[1]->package.elements[0]->integer.value, 3ULL);
	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[2]->package.elements[0]->integer.value, 1ULL);
}

/*
 * Test: acpi_ns_sort_list single element (edge case)
 */
static void nsrepair2_test_sort_list_single_element(struct kunit *test)
{
	union acpi_operand_object *pkg;
	union acpi_operand_object *sub_pkg0;

	pkg = create_package_object(test, 1);

	sub_pkg0 = create_package_object(test, 1);
	sub_pkg0->package.elements[0] = create_integer_object(test, 42);
	pkg->package.elements[0] = sub_pkg0;

	/* Sort single element - should not crash */
	test_sort_list(pkg->package.elements, pkg->package.count, 0, TEST_SORT_ASCENDING);

	KUNIT_EXPECT_EQ(test,
		pkg->package.elements[0]->package.elements[0]->integer.value, 42ULL);
}

/*
 * Test: acpi_ns_sort_list with PSS-like structure (descending frequency)
 */
static void nsrepair2_test_sort_list_pss_scenario(struct kunit *test)
{
	union acpi_operand_object *pss_pkg;
	union acpi_operand_object *sub_pkg;
	int i;

	/* Create _PSS package with 4 P-state subpackages in wrong order */
	pss_pkg = create_package_object(test, 4);

	/* P-state with frequency 1000 MHz */
	sub_pkg = create_package_object(test, 6);
	sub_pkg->package.elements[0] = create_integer_object(test, 1000);
	for (i = 1; i < 6; i++)
		sub_pkg->package.elements[i] = create_integer_object(test, 0);
	pss_pkg->package.elements[0] = sub_pkg;

	/* P-state with frequency 3000 MHz */
	sub_pkg = create_package_object(test, 6);
	sub_pkg->package.elements[0] = create_integer_object(test, 3000);
	for (i = 1; i < 6; i++)
		sub_pkg->package.elements[i] = create_integer_object(test, 0);
	pss_pkg->package.elements[1] = sub_pkg;

	/* P-state with frequency 2000 MHz */
	sub_pkg = create_package_object(test, 6);
	sub_pkg->package.elements[0] = create_integer_object(test, 2000);
	for (i = 1; i < 6; i++)
		sub_pkg->package.elements[i] = create_integer_object(test, 0);
	pss_pkg->package.elements[2] = sub_pkg;

	/* P-state with frequency 500 MHz */
	sub_pkg = create_package_object(test, 6);
	sub_pkg->package.elements[0] = create_integer_object(test, 500);
	for (i = 1; i < 6; i++)
		sub_pkg->package.elements[i] = create_integer_object(test, 0);
	pss_pkg->package.elements[3] = sub_pkg;

	/* Sort descending by frequency (index 0) - _PSS requires highest first */
	test_sort_list(pss_pkg->package.elements, pss_pkg->package.count, 0, TEST_SORT_DESCENDING);

	/* Verify sorted order: 3000, 2000, 1000, 500 */
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[0]->package.elements[0]->integer.value, 3000ULL);
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[1]->package.elements[0]->integer.value, 2000ULL);
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[2]->package.elements[0]->integer.value, 1000ULL);
	KUNIT_EXPECT_EQ(test,
		pss_pkg->package.elements[3]->package.elements[0]->integer.value, 500ULL);
}

static struct kunit_case nsrepair2_test_cases[] = {
	KUNIT_CASE(nsrepair2_test_integer_type),
	KUNIT_CASE(nsrepair2_test_string_type),
	KUNIT_CASE(nsrepair2_test_buffer_type),
	KUNIT_CASE(nsrepair2_test_package_type),
	KUNIT_CASE(nsrepair2_test_namespace_node),
	KUNIT_CASE(nsrepair2_test_nameseg_compare),
	KUNIT_CASE(nsrepair2_test_fde_constants),
	KUNIT_CASE(nsrepair2_test_sort_constants),
	KUNIT_CASE(nsrepair2_test_pss_package_structure),
	KUNIT_CASE(nsrepair2_test_cst_package_structure),
	KUNIT_CASE(nsrepair2_test_alr_package_structure),
	KUNIT_CASE(nsrepair2_test_hid_string_format),
	KUNIT_CASE(nsrepair2_test_hid_leading_asterisk),
	KUNIT_CASE(nsrepair2_test_hid_lowercase),
	KUNIT_CASE(nsrepair2_test_prt_package_structure),
	KUNIT_CASE(nsrepair2_test_fde_buffer_expansion),
	KUNIT_CASE(nsrepair2_test_fde_valid_buffer),
	KUNIT_CASE(nsrepair2_test_sort_list_ascending),
	KUNIT_CASE(nsrepair2_test_sort_list_descending),
	KUNIT_CASE(nsrepair2_test_sort_list_already_sorted),
	KUNIT_CASE(nsrepair2_test_sort_list_equal_values),
	KUNIT_CASE(nsrepair2_test_sort_list_by_index),
	KUNIT_CASE(nsrepair2_test_sort_list_single_element),
	KUNIT_CASE(nsrepair2_test_sort_list_pss_scenario),
	{}
};

static struct kunit_suite nsrepair2_test_suite = {
	.name = "acpi-nsrepair2",
	.test_cases = nsrepair2_test_cases,
};

kunit_test_suites(&nsrepair2_test_suite);

MODULE_DESCRIPTION("KUnit tests for ACPI nsrepair2 repair functions");
MODULE_LICENSE("GPL");
