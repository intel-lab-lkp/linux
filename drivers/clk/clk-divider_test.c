// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for clk_divider_bestdiv()
 */
#include <kunit/test.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/limits.h>
#include <linux/units.h>

#define PARENT_RATE_1GHZ	GIGA
#define PARENT_RATE_2GHZ	(2 * GIGA)
#define PARENT_RATE_4GHZ	(4 * GIGA)

static u32 fake_reg_a, fake_reg_b;

static const struct clk_div_table no_div1_table[] = {
	{0, 2},
	{1, 4},
	{2, 8},
	{0, 0},
};

static void unregister_fixed_rate(void *hw)
{
	clk_hw_unregister_fixed_rate(hw);
}

static void unregister_divider(void *hw)
{
	clk_hw_unregister_divider(hw);
}

static void unregister_mux(void *hw)
{
	clk_hw_unregister_mux(hw);
}

/*
 * Test that clk_round_rate(clk, ULONG_MAX) returns the maximum achievable
 * rate for a divider clock.
 */
static void clk_divider_bestdiv_ulong_max_returns_max_rate(struct kunit *test)
{
	struct clk_hw *parent_hw, *div_hw;
	unsigned long rate;

	parent_hw = clk_hw_register_fixed_rate(NULL, "bestdiv-parent",
					       NULL, 0, PARENT_RATE_1GHZ);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent_hw);
	kunit_add_action(test, unregister_fixed_rate, parent_hw);

	fake_reg_a = 0;
	div_hw = clk_hw_register_divider_table(NULL, "bestdiv-div",
					       "bestdiv-parent",
					       CLK_SET_RATE_PARENT,
					       (void __iomem *)&fake_reg_a,
					       0, 2, 0, no_div1_table, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, div_hw);
	kunit_add_action(test, unregister_divider, div_hw);

	/*
	 * ULONG_MAX is the canonical way to probe the maximum rate a clock
	 * can produce. With a parent at 1 GHz and the smallest table divider
	 * being 2, the expected maximum is 500 MHz.
	 *
	 * Before the fix this returned 125 MHz (PARENT_RATE / 8), the
	 * minimum rate, because the search loop was bypassed entirely.
	 */
	rate = clk_hw_round_rate(div_hw, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, rate, PARENT_RATE_1GHZ / 2);
}

/*
 * Test that clk_round_rate(clk, ULONG_MAX) returns the correct maximum rate when
 * a mux clock sits between a divider and its parent candidates.
 *
 * Topology:
 *
 *   [fixed 4 GHz] --\
 *                    +--> [mux CLK_SET_RATE_PARENT] --> [div {2,4,8} CLK_SET_RATE_PARENT]
 *   [fixed 2 GHz] --/
 *
 */
static void clk_divider_bestdiv_mux_ulong_max_returns_max_rate(struct kunit *test)
{
	static const char *const mux_parents[] = {
		"bestdiv-mux-parent-a",
		"bestdiv-mux-parent-b",
	};
	struct clk_hw *parent_a_hw, *parent_b_hw, *mux_hw, *div_hw;
	unsigned long rate;

	/* Higher-rate parent: the mux should select this for ULONG_MAX. */
	parent_a_hw = clk_hw_register_fixed_rate(NULL, "bestdiv-mux-parent-a",
						 NULL, 0, PARENT_RATE_4GHZ);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent_a_hw);
	kunit_add_action(test, unregister_fixed_rate, parent_a_hw);

	/* Lower-rate parent: should not be selected. */
	parent_b_hw = clk_hw_register_fixed_rate(NULL, "bestdiv-mux-parent-b",
						 NULL, 0, PARENT_RATE_2GHZ);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, parent_b_hw);
	kunit_add_action(test, unregister_fixed_rate, parent_b_hw);

	/*
	 * 1-bit mux register selects between the two parents.
	 * CLK_SET_RATE_PARENT allows the divider's rate request to
	 * propagate into clk_mux_determine_rate().
	 */
	fake_reg_a = 0;
	mux_hw = clk_hw_register_mux(NULL, "bestdiv-mux",
				     mux_parents, ARRAY_SIZE(mux_parents),
				     CLK_SET_RATE_PARENT,
				     (void __iomem *)&fake_reg_a,
				     0, 1, 0, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mux_hw);
	kunit_add_action(test, unregister_mux, mux_hw);

	fake_reg_b = 0;
	div_hw = clk_hw_register_divider_table(NULL, "bestdiv-mux-div",
					       "bestdiv-mux",
					       CLK_SET_RATE_PARENT,
					       (void __iomem *)&fake_reg_b,
					       0, 2, 0, no_div1_table, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, div_hw);
	kunit_add_action(test, unregister_divider, div_hw);

	/*
	 * Expected maximum: mux selects the 4 GHz parent, divider applies
	 * the smallest table entry (2): 4 GHz / 2 = 2 GHz.
	 */
	rate = clk_hw_round_rate(div_hw, ULONG_MAX);
	KUNIT_EXPECT_EQ(test, rate, PARENT_RATE_4GHZ / 2);
}

static struct kunit_case clk_divider_bestdiv_test_cases[] = {
	KUNIT_CASE(clk_divider_bestdiv_ulong_max_returns_max_rate),
	KUNIT_CASE(clk_divider_bestdiv_mux_ulong_max_returns_max_rate),
	{}
};

static struct kunit_suite clk_divider_bestdiv_test_suite = {
	.name = "clk_divider_bestdiv",
	.test_cases = clk_divider_bestdiv_test_cases,
};

kunit_test_suite(clk_divider_bestdiv_test_suite);

MODULE_DESCRIPTION("KUnit tests for clk_divider_bestdiv()");
MODULE_LICENSE("GPL");
