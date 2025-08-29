// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2025 Intel Corporation. All rights reserved.

#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <cxlmem.h>

#include "../cxl_test.h"

/* Maximum number of test vectors and entry length */
#define MAX_TABLE_ENTRIES 128
#define MAX_ENTRY_LEN 128

/* Expected number of parameters in each test vector */
#define EXPECTED_PARAMS 7

/* Module parameters for test vectors */
static char *table[MAX_TABLE_ENTRIES];
static int table_num;

/* Interleave Arithmetic */
#define MODULO_MATH 0
#define XOR_MATH 1

/*
 * XOR mapping configuration
 * The test data sets all use the same set of xormaps. When additional
 * data sets arrive for validation, this static setup will need to
 * be changed to accept xormaps as additional parameters.
 */
struct cxl_cxims_data *cximsd;
static u64 xormaps[] = {
	0x2020900,
	0x4041200,
	0x1010400,
	0x800,
};

static int nr_maps = ARRAY_SIZE(xormaps);

/**
 * to_hpa - calculate an HPA offset from a DPA offset and position
 *
 * dpa_offset: device physical address offset
 * pos: devices position in interleave
 * r_eiw: region encoded interleave ways
 * r_eig: region encoded interleave granularity
 * hb_ways: host bridge interleave ways
 * math: interleave arithmetic (MODULO_MATH or XOR_MATH)
 *
 * Returns: host physical address offset
 */
static u64 to_hpa(u64 dpa_offset, int pos, u8 r_eiw, u16 r_eig, u8 hb_ways,
		  u8 math)
{
	u64 hpa_offset;

	/* Calculate base HPA offset from DPA and position */
	hpa_offset = cxl_calculate_hpa_offset(dpa_offset, pos, r_eiw, r_eig);

	/* Apply XOR mapping if specified */
	if (math == XOR_MATH)
		hpa_offset = cxl_do_xormap_calc(cximsd, hpa_offset, hb_ways);

	return hpa_offset;
}

/**
 * to_dpa - translate an HPA offset to DPA offset
 *
 * hpa_offset: host physical address offset
 * r_eiw: region encoded interleave ways
 * r_eig: region encoded interleave granularity
 * hb_ways: host bridge interleave ways
 * math: interleave arithmetic (MODULO_MATH or XOR_MATH)
 *
 * Returns: device physical address offset
 */
static u64 to_dpa(u64 hpa_offset, u8 r_eiw, u16 r_eig, u8 hb_ways, u8 math)
{
	u64 offset = hpa_offset;

	/* Reverse XOR mapping if specified */
	if (math == XOR_MATH)
		offset = cxl_do_xormap_calc(cximsd, hpa_offset, hb_ways);

	return cxl_calculate_dpa_offset(offset, r_eiw, r_eig);
}

/**
 * to_pos - extract an interleave position from an HPA offset
 *
 * hpa_offset: host physical address offset
 * r_eiw: region encoded interleave ways
 * r_eig: region encoded interleave granularity
 * hb_ways: host bridge interleave ways
 * math: interleave arithmetic (MODULO_MATH or XOR_MATH)
 *
 * Returns: devices position in region interleave
 */
static u64 to_pos(u64 hpa_offset, u8 r_eiw, u16 r_eig, u8 hb_ways, u8 math)
{
	u64 offset = hpa_offset;

	/* Reverse XOR mapping if specified */
	if (math == XOR_MATH)
		offset = cxl_do_xormap_calc(cximsd, hpa_offset, hb_ways);

	return cxl_calculate_position(offset, r_eiw, r_eig);
}

/**
 * run_translation_test - execute forward and reverse translations
 *
 * @dpa: device physical address
 * @pos: expected position in region interleave
 * @r_eiw: region encoded interleave ways
 * @r_eig: region encoded interleave granularity
 * @hb_ways: host bridge interleave ways
 * @math: interleave arithmetic (MODULO_MATH or XOR_MATH)
 * @expect_spa: expected system physical address
 *
 * Returns: 0 on success, -1 on failure
 */
static int run_translation_test(u64 dpa, int pos, u8 r_eiw, u16 r_eig,
				u8 hb_ways, int math, u64 expect_hpa)
{
	u64 translated_spa, reverse_dpa;
	int reverse_pos;

	/* Test Device to Host translation: DPA + POS -> SPA */
	translated_spa = to_hpa(dpa, pos, r_eiw, r_eig, hb_ways, math);
	if (translated_spa != expect_hpa) {
		pr_err("Device to host failed: expected HPA %llu, got %llu\n",
		       expect_hpa, translated_spa);
		return -1;
	}

	/* Test Host to Device DPA translation: SPA -> DPA */
	reverse_dpa = to_dpa(translated_spa, r_eiw, r_eig, hb_ways, math);
	if (reverse_dpa != dpa) {
		pr_err("Host to Device DPA failed: expected %llu, got %llu\n",
		       dpa, reverse_dpa);
		return -1;
	}

	/* Test Host to Device Position translation: SPA -> POS */
	reverse_pos = to_pos(translated_spa, r_eiw, r_eig, hb_ways, math);
	if (reverse_pos != pos) {
		pr_err("Position lookup failed: expected %d, got %d\n", pos,
		       reverse_pos);
		return -1;
	}

	return 0;
}

/**
 * parse_test_vector - parse a single test vector string
 *
 * entry: test vector string to parse
 * dpa: device physical address
 * pos: expected position in region interleave
 * r_eiw: region encoded interleave ways
 * r_eig: region encoded interleave granularity
 * hb_ways: host bridge interleave ways
 * math: interleave arithmetic (MODULO_MATH or XOR_MATH)
 * expect_spa: expected system physical address
 *
 * Returns: 0 on success, negative error code on failure
 */
static int parse_test_vector(const char *entry, u64 *dpa, int *pos, u8 *r_eiw,
			     u16 *r_eig, u8 *hb_ways, int *math,
			     u64 *expect_hpa)
{
	unsigned int tmp_r_eiw, tmp_r_eig, tmp_hb_ways;
	int parsed;

	parsed = sscanf(entry, "%llu %d %u %u %u %d %llu", dpa, pos, &tmp_r_eiw,
			&tmp_r_eig, &tmp_hb_ways, math, expect_hpa);

	if (parsed != EXPECTED_PARAMS) {
		pr_err("Parse error: expected %d parameters, got %d in '%s'\n",
		       EXPECTED_PARAMS, parsed, entry);
		return -EINVAL;
	}
	if (tmp_r_eiw > U8_MAX || tmp_r_eig > U16_MAX || tmp_hb_ways > U8_MAX) {
		pr_err("Parameter overflow in entry: '%s'\n", entry);
		return -ERANGE;
	}
	if (*math != MODULO_MATH && *math != XOR_MATH) {
		pr_err("Invalid math type %d in entry: '%s'\n", *math, entry);
		return -EINVAL;
	}
	*r_eiw = tmp_r_eiw;
	*r_eig = tmp_r_eig;
	*hb_ways = tmp_hb_ways;

	return 0;
}

/*
 * setup_xor_mapping - Initialize XOR mapping data structure
 *
 * The test data sets all use the same set of xormaps. When additional
 * data sets arrive for validation, this static setup will need to
 * be changed to accept xormaps as additional parameters.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int setup_xor_mapping(void)
{
	if (nr_maps <= 0)
		return -EINVAL;

	cximsd = kzalloc(struct_size(cximsd, xormaps, nr_maps), GFP_KERNEL);
	if (!cximsd)
		return -ENOMEM;

	memcpy(cximsd->xormaps, xormaps, nr_maps * sizeof(*cximsd->xormaps));
	cximsd->nr_maps = nr_maps;

	return 0;
}

/*
 * cxl_translate_init - parse test vectors and kicks off translation tests
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __init cxl_translate_init(void)
{
	int ret, i;

	/* Validate module parameters */
	if (table_num == 0) {
		pr_err("No test vectors provided\n");
		return -EINVAL;
	}

	pr_info("CXL translate test module loaded with %d test vectors\n",
		table_num);

	ret = setup_xor_mapping();
	if (ret)
		return ret;

	/* Process each test vector */
	for (i = 0; i < table_num; i++) {
		u64 dpa, expect_spa;
		int pos, math;
		u8 r_eiw, hb_ways;
		u16 r_eig;

		pr_debug("Processing test vector %d: '%s'\n", i, table[i]);

		/* Parse the test vector */
		ret = parse_test_vector(table[i], &dpa, &pos, &r_eiw, &r_eig,
					&hb_ways, &math, &expect_spa);
		if (ret) {
			pr_err("CXL Translate Test %d: FAIL\n"
			       "    Failed to parse test vector '%s'\n",
			       i, table[i]);
			continue;
		}
		/* Run the translation test */
		ret = run_translation_test(dpa, pos, r_eiw, r_eig, hb_ways,
					   math, expect_spa);
		if (ret) {
			pr_err("CXL Translate Test %d: FAIL\n"
			       "    dpa=%llu pos=%d r_eiw=%u r_eig=%u hb_ways=%u math=%s expect_spa=%llu\n",
			       i, dpa, pos, r_eiw, r_eig, hb_ways,
			       (math == XOR_MATH) ? "XOR" : "MODULO",
			       expect_spa);
		} else {
			pr_info("CXL Translate Test %d: PASS\n", i);
		}
	}

	pr_info("CXL translate test completed\n");
	return 0;
}

static void __exit cxl_translate_exit(void)
{
	kfree(cximsd);

	pr_info("CXL translate test module unloaded\n");
}

module_param_array(table, charp, &table_num, 0444);
MODULE_PARM_DESC(table, "Test vectors as space-separated decimal strings");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("cxl_test: cxl address translation test module");
MODULE_IMPORT_NS("CXL");

module_init(cxl_translate_init);
module_exit(cxl_translate_exit);
