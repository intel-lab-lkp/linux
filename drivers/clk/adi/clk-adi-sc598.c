// SPDX-License-Identifier: GPL-2.0
/*
 * Clock support for ADI processor
 *
 * Copyright 2022-2024 - Analog Devices Inc.
 */

#include <dt-bindings/clock/adi-sc5xx-clock.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of_address.h>

#include "clk.h"

struct clk_core {
	const char		*name;
	const struct clk_ops	*ops;
	struct clk_hw		*hw;
	struct module		*owner;
	struct device		*dev;
	struct hlist_node	rpm_node;
	struct device_node	*of_node;
	struct clk_core		*parent;
	struct clk_parent_map	*parents;
	u8			num_parents;
	u8			new_parent_index;
	unsigned long		rate;
	unsigned long		req_rate;
	unsigned long		new_rate;
	struct clk_core		*new_parent;
	struct clk_core		*new_child;
	unsigned long		flags;
	bool			orphan;
	bool			rpm_enabled;
	unsigned int		enable_count;
	unsigned int		prepare_count;
	unsigned int		protect_count;
	unsigned long		min_rate;
	unsigned long		max_rate;
	unsigned long		accuracy;
	int			phase;
	struct clk_duty		duty;
	struct hlist_head	children;
	struct hlist_node	child_node;
	struct hlist_head	clks;
	unsigned int		notifier_count;
#ifdef CONFIG_DEBUG_FS
	struct dentry		*dentry;
	struct hlist_node	debug_node;
#endif
	struct kref		ref;
};

struct clk {
	struct clk_core	*core;
	struct device *dev;
	const char *dev_id;
	const char *con_id;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned int exclusive_count;
	struct hlist_node clks_node;
};

static DEFINE_SPINLOCK(cdu_lock);

static struct clk *clks[ADSP_SC598_CLK_END];
static struct clk_onecell_data clk_data;

static const char * const cgu1_in_sels[] = {"sys_clkin0", "sys_clkin1"};
static const char * const cgu0_s1sels[] = {"cgu0_s1seldiv", "cgu0_s1selexdiv"};
static const char * const cgu1_s0sels[] = {"cgu1_s0seldiv", "cgu1_s0selexdiv"};
static const char * const cgu1_s1sels[] = {"cgu1_s1seldiv", "cgu1_s1selexdiv"};
static const char * const sharc0_sels[] = {"cclk0_0", "dummy", "dummy", "dummy"};
static const char * const sharc1_sels[] = {"cclk0_0", "dummy", "dummy", "dummy"};
static const char * const arm_sels[] = {"dummy", "dummy", "cclk2_0", "cclk2_1"};
static const char * const cdu_ddr_sels[] = {"dclk_0", "dclk_1", "dummy", "dummy"};
static const char * const can_sels[] = {"dummy", "oclk_1", "dummy", "dummy"};
static const char * const spdif_sels[] = {"sclk1_0", "dummy", "dummy", "dummy"};
static const char * const spi_sels[] = {"sclk0_0", "oclk_0", "dummy", "dummy"};
static const char * const gige_sels[] = {"sclk0_0", "sclk0_1", "dummy", "dummy"};
static const char * const lp_sels[] = {"oclk_0", "sclk0_0", "cclk0_1", "dummy"};
static const char * const lp_ddr_sels[] = {"oclk_0", "dclk_0", "sysclk_1", "dummy"};
static const char * const ospi_refclk_sels[] = {"sysclk_0", "sclk0_0", "sclk1_1", "dummy"};
static const char * const trace_sels[] = {"sclk0_0", "dummy", "dummy", "dummy"};
static const char * const emmc_sels[] = {"oclk_0", "sclk0_1", "dclk_0_half", "dclk_1_half"};
static const char * const emmc_timer_sels[] = {"dummy", "sclk1_1_half", "dummy", "dummy"};

static const char * const ddr_sels[] = {"cdu_ddr", "3pll_ddiv"};

static void sc598_clock_setup(struct device_node *np)
{
	void __iomem *cgu0;
	void __iomem *cgu1;
	void __iomem *cdu;
	void __iomem *pll3;
	int i;

	cgu0 = of_iomap(np, 0);
	if (IS_ERR(cgu0)) {
		pr_err("Unable to remap CGU0 address (resource 0)\n");
		return;
	}

	cgu1 = of_iomap(np, 1);
	if (IS_ERR(cgu1)) {
		pr_err("Unable to remap CGU1 address (resource 1)\n");
		return;
	}

	cdu = of_iomap(np, 2);
	if (IS_ERR(cdu)) {
		pr_err("Unable to remap CDU address (resource 2)\n");
		return;
	}

	pll3 = of_iomap(np, 3);
	if (IS_ERR(pll3)) {
		pr_err("Unable to remap PLL3 control register (resource 3)\n");
		return;
	}

	// We only access this one register for pll3
	pll3 = pll3 + PLL3_OFFSET;

	// Input clock configuration
	clks[ADSP_SC598_CLK_DUMMY] = clk_register_fixed_rate(NULL, "dummy", NULL, 0, 0);
	clks[ADSP_SC598_CLK_SYS_CLKIN0] = of_clk_get_by_name(np, "sys_clkin0");
	clks[ADSP_SC598_CLK_SYS_CLKIN1] = of_clk_get_by_name(np, "sys_clkin1");
	clks[ADSP_SC598_CLK_CGU1_IN] = clk_register_mux(NULL, "cgu1_in_sel",
		cgu1_in_sels, 2, CLK_SET_RATE_PARENT, cdu + CDU_CLKINSEL, 0, 1, 0,
		&cdu_lock);

	// 3rd pll reuses cgu1 clk in selection, feeds directly into 3pll df
	// changing the cgu1 in sel mux will affect 3pll so reuse the same clocks

	// CGU configuration and internal clocks
	clks[ADSP_SC598_CLK_CGU0_PLL_IN] = clk_register_divider(NULL, "cgu0_df",
		"sys_clkin0", CLK_SET_RATE_PARENT, cgu0 + CGU_CTL, 0, 1, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_PLL_IN] = clk_register_divider(NULL, "cgu1_df",
		"cgu1_in_sel", CLK_SET_RATE_PARENT, cgu1 + CGU_CTL, 0, 1, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_3PLL_PLL_IN] = clk_register_divider(NULL, "3pll_df",
		"cgu1_in_sel", CLK_SET_RATE_PARENT, pll3, 3, 1, 0, &cdu_lock);

	// VCO output inside PLL
	clks[ADSP_SC598_CLK_CGU0_VCO_OUT] = sc5xx_cgu_pll("cgu0_vco_msel", "cgu0_df",
		cgu0 + CGU_CTL, CGU_MSEL_SHIFT, CGU_MSEL_WIDTH, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_VCO_OUT] = sc5xx_cgu_pll("cgu1_vco_msel", "cgu1_df",
		cgu1 + CGU_CTL, CGU_MSEL_SHIFT, CGU_MSEL_WIDTH, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_3PLL_VCO_OUT] = sc5xx_cgu_pll("3pll_vco_msel", "3pll_df",
		pll3, PLL3_MSEL_SHIFT, PLL3_MSEL_WIDTH, 1, &cdu_lock);

	//MSEL constant factor
	clks[ADSP_SC598_CLK_CGU0_VCO_2_OUT] = clk_register_fixed_factor(NULL,
		"cgu0_vco", "cgu0_vco_msel", CLK_SET_RATE_PARENT, 2, 1);
	clks[ADSP_SC598_CLK_CGU1_VCO_2_OUT] = clk_register_fixed_factor(NULL,
		"cgu1_vco", "cgu0_vco_msel", CLK_SET_RATE_PARENT, 2, 1);
	clks[ADSP_SC598_CLK_3PLL_VCO_2_OUT] = clk_register_fixed_factor(NULL,
		"3pll_vco", "cgu0_vco_msel", CLK_SET_RATE_PARENT, 2, 1);

	// Final PLL output
	clks[ADSP_SC598_CLK_CGU0_PLLCLK] = clk_register_fixed_factor(NULL,
		"cgu0_pllclk", "cgu0_vco", CLK_SET_RATE_PARENT, 1, 2);
	clks[ADSP_SC598_CLK_CGU1_PLLCLK] = clk_register_fixed_factor(NULL,
		"cgu1_pllclk", "cgu1_vco", CLK_SET_RATE_PARENT, 1, 2);
	clks[ADSP_SC598_CLK_3PLL_PLLCLK] = clk_register_fixed_factor(NULL,
		"3pll_pllclk", "3pll_vco", CLK_SET_RATE_PARENT, 1, 2);

	// Dividers from pll output
	clks[ADSP_SC598_CLK_CGU0_CDIV] = cgu_divider("cgu0_cdiv", "cgu0_pllclk",
		cgu0 + CGU_DIV, 0, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_SYSCLK] = cgu_divider("sysclk_0", "cgu0_pllclk",
		cgu0 + CGU_DIV, 8, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_DDIV] = cgu_divider("cgu0_ddiv", "cgu0_pllclk",
		cgu0 + CGU_DIV, 16, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_ODIV] = cgu_divider("cgu0_odiv", "cgu0_pllclk",
		cgu0 + CGU_DIV, 22, 7, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_S0SELDIV] = cgu_divider("cgu0_s0seldiv",
		"sysclk_0", cgu0 + CGU_DIV, 5, 3, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_S1SELDIV] = cgu_divider("cgu0_s1seldiv",
		"sysclk_0", cgu0 + CGU_DIV, 13, 3, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_S1SELEXDIV] = cgu_divider("cgu0_s1selexdiv",
		"cgu0_pllclk", cgu0 + CGU_DIVEX, 16, 8, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_S1SEL] = clk_register_mux(NULL, "cgu0_sclk1sel",
		cgu0_s1sels, 2, CLK_SET_RATE_PARENT, cgu0 + CGU_CTL, 17, 1, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_CCLK2] = clk_register_fixed_factor(NULL,
		"cclk2_0", "cgu0_vco", CLK_SET_RATE_PARENT, 1, 3);

	clks[ADSP_SC598_CLK_CGU1_CDIV] = cgu_divider("cgu1_cdiv", "cgu1_pllclk",
		cgu1 + CGU_DIV, 0, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_SYSCLK] = cgu_divider("sysclk_1", "cgu1_pllclk",
		cgu1 + CGU_DIV, 8, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_DDIV] = cgu_divider("cgu1_ddiv", "cgu1_pllclk",
		cgu1 + CGU_DIV, 16, 5, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_ODIV] = cgu_divider("cgu1_odiv", "cgu1_pllclk",
		cgu1 + CGU_DIV, 22, 7, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S0SELDIV] = cgu_divider("cgu1_s0seldiv",
		"sysclk_1", cgu1 + CGU_DIV, 5, 3, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S1SELDIV] = cgu_divider("cgu1_s1seldiv",
		"sysclk_1", cgu1 + CGU_DIV, 13, 3, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S0SELEXDIV] = cgu_divider("cgu1_s0selexdiv",
		"cgu1_pllclk", cgu1 + CGU_DIVEX, 0, 8, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S1SELEXDIV] = cgu_divider("cgu1_s1selexdiv",
		"cgu1_pllclk", cgu1 + CGU_DIVEX, 16, 8, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S0SEL] = clk_register_mux(NULL, "cgu1_sclk0sel",
		cgu1_s0sels, 2, CLK_SET_RATE_PARENT, cgu1 + CGU_CTL, 16, 1, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_S1SEL] = clk_register_mux(NULL, "cgu1_sclk1sel",
		cgu1_s1sels, 2, CLK_SET_RATE_PARENT, cgu1 + CGU_CTL, 17, 1, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_CCLK2] = clk_register_fixed_factor(NULL,
		"cclk2_1", "cgu1_vco", CLK_SET_RATE_PARENT, 1, 3);

	clks[ADSP_SC598_CLK_3PLL_DDIV] = clk_register_divider(NULL, "3pll_ddiv",
		"3pll_pllclk", CLK_SET_RATE_PARENT, pll3, 12, 5, 0, &cdu_lock);

	// Gates to enable CGU outputs
	clks[ADSP_SC598_CLK_CGU0_CCLK0] = cgu_gate("cclk0_0", "cgu0_cdiv",
		cgu0 + CGU_CCBF_DIS, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_OCLK] = cgu_gate("oclk_0", "cgu0_odiv",
		cgu0 + CGU_SCBF_DIS, 3, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_DCLK] = cgu_gate("dclk_0", "cgu0_ddiv",
		cgu0 + CGU_SCBF_DIS, 2, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_SCLK1] = cgu_gate("sclk1_0", "cgu0_sclk1sel",
		cgu0 + CGU_SCBF_DIS, 1, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU0_SCLK0] = cgu_gate("sclk0_0", "cgu0_s0seldiv",
		cgu0 + CGU_SCBF_DIS, 0, &cdu_lock);

	clks[ADSP_SC598_CLK_CGU1_CCLK0] = cgu_gate("cclk0_1", "cgu1_cdiv",
		cgu1 + CGU_CCBF_DIS, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_OCLK] = cgu_gate("oclk_1", "cgu1_odiv",
		cgu1 + CGU_SCBF_DIS, 3, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_DCLK] = cgu_gate("dclk_1", "cgu1_ddiv",
		cgu1 + CGU_SCBF_DIS, 2, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_SCLK1] = cgu_gate("sclk1_1", "cgu1_sclk1sel",
		cgu1 + CGU_SCBF_DIS, 1, &cdu_lock);
	clks[ADSP_SC598_CLK_CGU1_SCLK0] = cgu_gate("sclk0_1", "cgu1_sclk0sel",
		cgu1 + CGU_SCBF_DIS, 0, &cdu_lock);

	// Extra half rate clocks generated in the CDU
	clks[ADSP_SC598_CLK_DCLK0_HALF] = clk_register_fixed_factor(NULL, "dclk_0_half",
		"dclk_0", CLK_SET_RATE_PARENT, 1, 2);
	clks[ADSP_SC598_CLK_DCLK1_HALF] = clk_register_fixed_factor(NULL, "dclk_1_half",
		"dclk_1", CLK_SET_RATE_PARENT, 1, 2);
	clks[ADSP_SC598_CLK_CGU1_SCLK1_HALF] = clk_register_fixed_factor(NULL,
		"sclk1_1_half", "sclk1_1", CLK_SET_RATE_PARENT, 1, 2);

	// CDU output muxes
	clks[ADSP_SC598_CLK_SHARC0_SEL] = cdu_mux("sharc0_sel", cdu + CDU_CFG0,
		sharc0_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_SHARC1_SEL] = cdu_mux("sharc1_sel", cdu + CDU_CFG1,
		sharc1_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_ARM_SEL] = cdu_mux("arm_sel", cdu + CDU_CFG2,
		arm_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_CDU_DDR_SEL] = cdu_mux("cdu_ddr_sel", cdu + CDU_CFG3,
		cdu_ddr_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_CAN_SEL] = cdu_mux("can_sel", cdu + CDU_CFG4,
		can_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_SPDIF_SEL] = cdu_mux("spdif_sel", cdu + CDU_CFG5,
		spdif_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_SPI_SEL] = cdu_mux("spi_sel", cdu + CDU_CFG6,
		spi_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_GIGE_SEL] = cdu_mux("gige_sel", cdu + CDU_CFG7,
		gige_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_LP_SEL] = cdu_mux("lp_sel", cdu + CDU_CFG8, lp_sels,
		&cdu_lock);
	clks[ADSP_SC598_CLK_LP_DDR_SEL] = cdu_mux("lp_ddr_sel", cdu + CDU_CFG9,
		lp_ddr_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_OSPI_REFCLK_SEL] = cdu_mux("ospi_refclk_sel",
		cdu + CDU_CFG10, ospi_refclk_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_TRACE_SEL] = cdu_mux("trace_sel", cdu + CDU_CFG12,
		trace_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_EMMC_SEL] = cdu_mux("emmc_sel", cdu + CDU_CFG13,
		emmc_sels, &cdu_lock);
	clks[ADSP_SC598_CLK_EMMC_TIMER_QMC_SEL] = cdu_mux("emmc_timer_qmc_sel",
		cdu + CDU_CFG14, emmc_timer_sels, &cdu_lock);

	// CDU output enable gates
	clks[ADSP_SC598_CLK_SHARC0] = cdu_gate("sharc0", "sharc0_sel",
		cdu + CDU_CFG0, CLK_IS_CRITICAL, &cdu_lock);
	clks[ADSP_SC598_CLK_SHARC1] = cdu_gate("sharc1", "sharc1_sel",
		cdu + CDU_CFG1, CLK_IS_CRITICAL, &cdu_lock);
	clks[ADSP_SC598_CLK_ARM] = cdu_gate("arm", "arm_sel", cdu + CDU_CFG2,
		CLK_IS_CRITICAL, &cdu_lock);
	clks[ADSP_SC598_CLK_CDU_DDR] = cdu_gate("cdu_ddr", "cdu_ddr_sel",
		cdu + CDU_CFG3, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_CAN] = cdu_gate("can", "can_sel", cdu + CDU_CFG4, 0,
		&cdu_lock);
	clks[ADSP_SC598_CLK_SPDIF] = cdu_gate("spdif", "spdif_sel", cdu + CDU_CFG5,
		0, &cdu_lock);
	clks[ADSP_SC598_CLK_SPI] = cdu_gate("spi", "spi_sel", cdu + CDU_CFG6, 0,
		&cdu_lock);
	clks[ADSP_SC598_CLK_GIGE] = cdu_gate("gige", "gige_sel", cdu + CDU_CFG7, 0,
		&cdu_lock);
	clks[ADSP_SC598_CLK_LP] = cdu_gate("lp", "lp_sel", cdu + CDU_CFG8, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_LP_DDR] = cdu_gate("lp_ddr", "lp_ddr_sel",
		cdu + CDU_CFG9, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_OSPI_REFCLK] = cdu_gate("ospi_refclk", "ospi_refclk_sel",
		cdu + CDU_CFG10, 0, &cdu_lock);
	clks[ADSP_SC598_CLK_TRACE] = cdu_gate("trace", "trace_sel", cdu + CDU_CFG12,
		0, &cdu_lock);
	clks[ADSP_SC598_CLK_EMMC] = cdu_gate("emmc", "emmc_sel", cdu + CDU_CFG13, 0,
		&cdu_lock);
	clks[ADSP_SC598_CLK_EMMC_TIMER_QMC] = cdu_gate("emmc_timer_qmc",
		"emmc_timer_qmc_sel", cdu + CDU_CFG14, 0, &cdu_lock);

	// Dedicated DDR output mux
	clks[ADSP_SC598_CLK_DDR] = clk_register_mux(NULL, "ddr", ddr_sels, 2,
		CLK_SET_RATE_PARENT | CLK_IS_CRITICAL, pll3, 11, 1, 0, &cdu_lock);

	for (i = 0; i < ARRAY_SIZE(clks); i++) {
		if (IS_ERR(clks[i])) {
			pr_err("Zynq clk %d: register failed with %ld\n",
			       i, PTR_ERR(clks[i]));
			BUG();
		}
	}

	clk_data.clks = clks;
	clk_data.clk_num = ARRAY_SIZE(clks);
	of_clk_add_provider(np, of_clk_src_onecell_get, &clk_data);
}

CLK_OF_DECLARE(sc598_clocks, "adi,sc598-clocks", sc598_clock_setup);

MODULE_AUTHOR("Greg Malysa <greg.malysa@timesys.com>");
MODULE_DESCRIPTION("Analog Devices Clock driver");
MODULE_LICENSE("GPL v2");