// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include <dt-bindings/clock/at91.h>

#include "pmc.h"

static DEFINE_SPINLOCK(mck_lock);

static const struct clk_master_characteristics mck_characteristics = {
	.output = { .min = 0, .max = 133333333 },
	.divisors = { 1, 2, 4, 3 },
	.have_div3_pres = 1,
};

static u8 plla_out[] = { 0, 1, 2, 3, 0, 1, 2, 3 };

static u16 plla_icpll[] = { 0, 0, 0, 0, 1, 1, 1, 1 };

static const struct clk_range plla_outputs[] = {
	{ .min = 745000000, .max = 800000000 },
	{ .min = 695000000, .max = 750000000 },
	{ .min = 645000000, .max = 700000000 },
	{ .min = 595000000, .max = 650000000 },
	{ .min = 545000000, .max = 600000000 },
	{ .min = 495000000, .max = 555000000 },
	{ .min = 445000000, .max = 500000000 },
	{ .min = 400000000, .max = 450000000 },
};

static const struct clk_pll_characteristics plla_characteristics = {
	.input = { .min = 2000000, .max = 32000000 },
	.num_output = ARRAY_SIZE(plla_outputs),
	.output = plla_outputs,
	.icpll = plla_icpll,
	.out = plla_out,
};

static struct {
	char *n;
	struct clk_hw *parent_hw;
	unsigned long flags;
	u8 id;
} at91sam9x5_systemck[] = {
	/*
	 * ddrck feeds DDR controller and is enabled by bootloader thus we need
	 * to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "ddrck", .id = 2, .flags = CLK_IS_CRITICAL },
	{ .n = "smdck", .id = 4 },
	{ .n = "uhpck", .id = 6 },
	{ .n = "udpck", .id = 7 },
	{ .n = "pck0",  .id = 8 },
	{ .n = "pck1",  .id = 9 },
};

static const struct clk_pcr_layout at91sam9x5_pcr_layout = {
	.offset = 0x10c,
	.cmd = BIT(12),
	.pid_mask = GENMASK(5, 0),
	.div_mask = GENMASK(17, 16),
};

struct pck {
	char *n;
	u8 id;
};

static const struct pck at91sam9x5_periphck[] = {
	{ .n = "pioAB_clk",  .id = 2, },
	{ .n = "pioCD_clk",  .id = 3, },
	{ .n = "smd_clk",    .id = 4, },
	{ .n = "usart0_clk", .id = 5, },
	{ .n = "usart1_clk", .id = 6, },
	{ .n = "usart2_clk", .id = 7, },
	{ .n = "twi0_clk",   .id = 9, },
	{ .n = "twi1_clk",   .id = 10, },
	{ .n = "twi2_clk",   .id = 11, },
	{ .n = "mci0_clk",   .id = 12, },
	{ .n = "spi0_clk",   .id = 13, },
	{ .n = "spi1_clk",   .id = 14, },
	{ .n = "uart0_clk",  .id = 15, },
	{ .n = "uart1_clk",  .id = 16, },
	{ .n = "tcb0_clk",   .id = 17, },
	{ .n = "pwm_clk",    .id = 18, },
	{ .n = "adc_clk",    .id = 19, },
	{ .n = "dma0_clk",   .id = 20, },
	{ .n = "dma1_clk",   .id = 21, },
	{ .n = "uhphs_clk",  .id = 22, },
	{ .n = "udphs_clk",  .id = 23, },
	{ .n = "mci1_clk",   .id = 26, },
	{ .n = "ssc0_clk",   .id = 28, },
};

static const struct pck at91sam9g15_periphck[] = {
	{ .n = "lcdc_clk", .id = 25, },
	{ /* sentinel */}
};

static const struct pck at91sam9g25_periphck[] = {
	{ .n = "usart3_clk", .id = 8, },
	{ .n = "macb0_clk", .id = 24, },
	{ .n = "isi_clk", .id = 25, },
	{ /* sentinel */}
};

static const struct pck at91sam9g35_periphck[] = {
	{ .n = "macb0_clk", .id = 24, },
	{ .n = "lcdc_clk", .id = 25, },
	{ /* sentinel */}
};

static const struct pck at91sam9x25_periphck[] = {
	{ .n = "usart3_clk", .id = 8, },
	{ .n = "macb0_clk", .id = 24, },
	{ .n = "macb1_clk", .id = 27, },
	{ .n = "can0_clk", .id = 29, },
	{ .n = "can1_clk", .id = 30, },
	{ /* sentinel */}
};

static const struct pck at91sam9x35_periphck[] = {
	{ .n = "macb0_clk", .id = 24, },
	{ .n = "lcdc_clk", .id = 25, },
	{ .n = "can0_clk", .id = 29, },
	{ .n = "can1_clk", .id = 30, },
	{ /* sentinel */}
};

static void __init at91sam9x5_pmc_setup(struct device_node *np,
					const struct pck *extra_pcks,
					bool has_lcdck)
{
	const char *slow_clk_name = "slowck", *main_xtal_name = "main_xtal";
	struct clk_hw *main_rc_hw, *main_osc_hw, *hw;
	u8 slow_clk_index = 0, main_xtal_index = 0;
	struct clk_range range = CLK_RANGE(0, 0);
	struct clk_parent_data parent_data[6];
	struct clk_hw *smdck_hw, *usbck_hw;
	struct pmc_data *at91sam9x5_pmc;
	struct regmap *regmap;
	bool bypass;
	int i;

	regmap = device_node_to_regmap(np);
	if (IS_ERR(regmap))
		return;

	at91sam9x5_pmc = pmc_data_allocate(PMC_PLLACK + 1,
					   nck(at91sam9x5_systemck), 31, 0, 2);
	if (!at91sam9x5_pmc)
		return;

	main_rc_hw = at91_clk_register_main_rc_osc(regmap, "main_rc_osc", 12000000,
						   50000000);
	if (IS_ERR(main_rc_hw))
		goto err_free;

	bypass = of_property_read_bool(np, "atmel,osc-bypass");

	main_osc_hw = at91_clk_register_main_osc(regmap, "main_osc", NULL,
						 &AT91_CLK_PD_NAME(main_xtal_name, main_xtal_index),
						 bypass);
	if (IS_ERR(main_osc_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_HW(main_rc_hw);
	parent_data[1] = AT91_CLK_PD_HW(main_osc_hw);
	hw = at91_clk_register_sam9x5_main(regmap, "mainck", NULL, parent_data, 2);
	if (IS_ERR(hw))
		goto err_free;

	at91sam9x5_pmc->chws[PMC_MAIN] = hw;

	hw = at91_clk_register_pll(regmap, "pllack", NULL,
				   &AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MAIN]), 0,
				   &at91rm9200_pll_layout, &plla_characteristics);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_plldiv(regmap, "plladivck", NULL, &AT91_CLK_PD_HW(hw));
	if (IS_ERR(hw))
		goto err_free;

	at91sam9x5_pmc->chws[PMC_PLLACK] = hw;

	hw = at91_clk_register_utmi(regmap, NULL, "utmick", NULL,
				    &AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MAIN]));
	if (IS_ERR(hw))
		goto err_free;

	at91sam9x5_pmc->chws[PMC_UTMI] = hw;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MAIN]);
	parent_data[2] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_UTMI]);
	hw = at91_clk_register_master_pres(regmap, "masterck_pres", 4,
					   NULL, parent_data,
					   &at91sam9x5_master_layout,
					   &mck_characteristics, &mck_lock);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_master_div(regmap, "masterck_div", NULL,
					  &AT91_CLK_PD_HW(hw),
					  &at91sam9x5_master_layout,
					  &mck_characteristics, &mck_lock,
					  CLK_SET_RATE_GATE, 0);
	if (IS_ERR(hw))
		goto err_free;

	at91sam9x5_pmc->chws[PMC_MCK] = hw;

	parent_data[0] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_UTMI]);
	usbck_hw = at91sam9x5_clk_register_usb(regmap, "usbck", NULL, parent_data, 2);
	if (IS_ERR(usbck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_UTMI]);
	smdck_hw = at91sam9x5_clk_register_smd(regmap, "smdclk", NULL, parent_data, 2);
	if (IS_ERR(smdck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MAIN]);
	parent_data[2] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_UTMI]);
	parent_data[4] = AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MCK]);
	for (i = 0; i < 2; i++) {
		char name[6];

		snprintf(name, sizeof(name), "prog%d", i);

		hw = at91_clk_register_programmable(regmap, name,
						    NULL, parent_data, 5, i,
						    &at91sam9x5_programmable_layout,
						    NULL);
		if (IS_ERR(hw))
			goto err_free;

		at91sam9x5_pmc->pchws[i] = hw;
	}

	/* Set systemck parent hws. */
	at91sam9x5_systemck[0].parent_hw = at91sam9x5_pmc->chws[PMC_MCK];
	at91sam9x5_systemck[1].parent_hw = smdck_hw;
	at91sam9x5_systemck[2].parent_hw = usbck_hw;
	at91sam9x5_systemck[3].parent_hw = usbck_hw;
	at91sam9x5_systemck[4].parent_hw = at91sam9x5_pmc->pchws[0];
	at91sam9x5_systemck[5].parent_hw = at91sam9x5_pmc->pchws[1];
	for (i = 0; i < ARRAY_SIZE(at91sam9x5_systemck); i++) {
		hw = at91_clk_register_system(regmap, at91sam9x5_systemck[i].n, NULL,
					      &AT91_CLK_PD_HW(at91sam9x5_systemck[i].parent_hw),
					      at91sam9x5_systemck[i].id,
					      at91sam9x5_systemck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		at91sam9x5_pmc->shws[at91sam9x5_systemck[i].id] = hw;
	}

	if (has_lcdck) {
		hw = at91_clk_register_system(regmap, "lcdck", NULL,
					      &AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MCK]), 3, 0);
		if (IS_ERR(hw))
			goto err_free;

		at91sam9x5_pmc->shws[3] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(at91sam9x5_periphck); i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &at91sam9x5_pcr_layout,
							 at91sam9x5_periphck[i].n,
							 NULL,
							 &AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MCK]),
							 at91sam9x5_periphck[i].id,
							 &range, INT_MIN, 0);
		if (IS_ERR(hw))
			goto err_free;

		at91sam9x5_pmc->phws[at91sam9x5_periphck[i].id] = hw;
	}

	for (i = 0; extra_pcks[i].id; i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &at91sam9x5_pcr_layout,
							 extra_pcks[i].n,
							 NULL,
							 &AT91_CLK_PD_HW(at91sam9x5_pmc->chws[PMC_MCK]),
							 extra_pcks[i].id,
							 &range, INT_MIN, 0);
		if (IS_ERR(hw))
			goto err_free;

		at91sam9x5_pmc->phws[extra_pcks[i].id] = hw;
	}

	of_clk_add_hw_provider(np, of_clk_hw_pmc_get, at91sam9x5_pmc);

	return;

err_free:
	kfree(at91sam9x5_pmc);
}

static void __init at91sam9g15_pmc_setup(struct device_node *np)
{
	at91sam9x5_pmc_setup(np, at91sam9g15_periphck, true);
}

CLK_OF_DECLARE(at91sam9g15_pmc, "atmel,at91sam9g15-pmc", at91sam9g15_pmc_setup);

static void __init at91sam9g25_pmc_setup(struct device_node *np)
{
	at91sam9x5_pmc_setup(np, at91sam9g25_periphck, false);
}

CLK_OF_DECLARE(at91sam9g25_pmc, "atmel,at91sam9g25-pmc", at91sam9g25_pmc_setup);

static void __init at91sam9g35_pmc_setup(struct device_node *np)
{
	at91sam9x5_pmc_setup(np, at91sam9g35_periphck, true);
}

CLK_OF_DECLARE(at91sam9g35_pmc, "atmel,at91sam9g35-pmc", at91sam9g35_pmc_setup);

static void __init at91sam9x25_pmc_setup(struct device_node *np)
{
	at91sam9x5_pmc_setup(np, at91sam9x25_periphck, false);
}

CLK_OF_DECLARE(at91sam9x25_pmc, "atmel,at91sam9x25-pmc", at91sam9x25_pmc_setup);

static void __init at91sam9x35_pmc_setup(struct device_node *np)
{
	at91sam9x5_pmc_setup(np, at91sam9x35_periphck, true);
}

CLK_OF_DECLARE(at91sam9x35_pmc, "atmel,at91sam9x35-pmc", at91sam9x35_pmc_setup);
