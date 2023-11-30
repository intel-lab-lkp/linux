// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Intel Corporation
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

/* a tiny module only meant to test
 *
 *   set/clear_bit
 *   get_count_order/long
 */

/* use an enum because that's the most common BITMAP usage */
enum bitops_fun {
	BITOPS_4 = 4,
	BITOPS_7 = 7,
	BITOPS_11 = 11,
	BITOPS_31 = 31,
	BITOPS_88 = 88,
	BITOPS_LAST = 255,
	BITOPS_LENGTH = 256
};

static DECLARE_BITMAP(g_bitmap, BITOPS_LENGTH);

static unsigned int order_comb[][2] = {
	{0x00000003,  2},
	{0x00000004,  2},
	{0x00001fff, 13},
	{0x00002000, 13},
	{0x50000000, 31},
	{0x80000000, 31},
	{0x80003000, 32},
};

#ifdef CONFIG_64BIT
static unsigned long order_comb_long[][2] = {
	{0x0000000300000000, 34},
	{0x0000000400000000, 34},
	{0x00001fff00000000, 45},
	{0x0000200000000000, 45},
	{0x5000000000000000, 63},
	{0x8000000000000000, 63},
	{0x8000300000000000, 64},
};
#endif

/* Assert that a boolean expression can be folded in a constant and is true. */
#define test_const_eval(test_expr)				\
({								\
	/* Evaluate once so that compiler can fold it. */	\
	bool __test_expr = test_expr;				\
								\
	BUILD_BUG_ON(!__builtin_constant_p(__test_expr));	\
	BUILD_BUG_ON(!__test_expr);				\
})

static void test_bitops_const_eval(void)
{
	/*
	 * On any supported optimization level (-O2, -Os) and if
	 * invoked with a compile-time constant argument, the compiler
	 * must be able to fold into a constant expression all the bit
	 * find functions. Namely: __ffs(), ffs(), ffz(), __fls(),
	 * fls() and fls64(). Otherwise, trigger a build bug.
	 */
	const int n = 10;

	test_const_eval(__ffs(BIT(n)) == n);
	test_const_eval(ffs(BIT(n)) == n + 1);
	test_const_eval(ffz(~BIT(n)) == n);
	test_const_eval(__fls(BIT(n)) == n);
	test_const_eval(fls(BIT(n)) == n + 1);
	test_const_eval(fls64(BIT_ULL(n)) == n + 1);
}

static int __init test_bitops_startup(void)
{
	int i, bit_set;

	pr_info("Starting bitops test\n");
	set_bit(BITOPS_4, g_bitmap);
	set_bit(BITOPS_7, g_bitmap);
	set_bit(BITOPS_11, g_bitmap);
	set_bit(BITOPS_31, g_bitmap);
	set_bit(BITOPS_88, g_bitmap);

	for (i = 0; i < ARRAY_SIZE(order_comb); i++) {
		if (order_comb[i][1] != get_count_order(order_comb[i][0]))
			pr_warn("get_count_order wrong for %x\n",
				       order_comb[i][0]);
	}

	for (i = 0; i < ARRAY_SIZE(order_comb); i++) {
		if (order_comb[i][1] != get_count_order_long(order_comb[i][0]))
			pr_warn("get_count_order_long wrong for %x\n",
				       order_comb[i][0]);
	}

#ifdef CONFIG_64BIT
	for (i = 0; i < ARRAY_SIZE(order_comb_long); i++) {
		if (order_comb_long[i][1] !=
			       get_count_order_long(order_comb_long[i][0]))
			pr_warn("get_count_order_long wrong for %lx\n",
				       order_comb_long[i][0]);
	}
#endif

	barrier();

	clear_bit(BITOPS_4, g_bitmap);
	clear_bit(BITOPS_7, g_bitmap);
	clear_bit(BITOPS_11, g_bitmap);
	clear_bit(BITOPS_31, g_bitmap);
	clear_bit(BITOPS_88, g_bitmap);

	bit_set = find_first_bit(g_bitmap, BITOPS_LAST);
	if (bit_set != BITOPS_LAST)
		pr_err("ERROR: FOUND SET BIT %d\n", bit_set);

	test_bitops_const_eval();

	pr_info("Completed bitops test\n");

	return 0;
}

static void __exit test_bitops_unstartup(void)
{
}

module_init(test_bitops_startup);
module_exit(test_bitops_unstartup);

MODULE_AUTHOR("Jesse Brandeburg <jesse.brandeburg@intel.com>, Wei Yang <richard.weiyang@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bit testing module");
