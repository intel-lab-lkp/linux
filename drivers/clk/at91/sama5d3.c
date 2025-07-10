// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include <dt-bindings/clock/at91.h>

#include "pmc.h"

static DEFINE_SPINLOCK(mck_lock);

static const struct clk_master_characteristics mck_characteristics = {
	.output = { .min = 0, .max = 166000000 },
	.divisors = { 1, 2, 4, 3 },
};

static u8 plla_out[] = { 0 };

static u16 plla_icpll[] = { 0 };

static const struct clk_range plla_outputs[] = {
	{ .min = 400000000, .max = 1000000000 },
};

static const struct clk_pll_characteristics plla_characteristics = {
	.input = { .min = 8000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(plla_outputs),
	.output = plla_outputs,
	.icpll = plla_icpll,
	.out = plla_out,
};

static const struct clk_pcr_layout sama5d3_pcr_layout = {
	.offset = 0x10c,
	.cmd = BIT(12),
	.pid_mask = GENMASK(6, 0),
	.div_mask = GENMASK(17, 16),
};

static struct {
	char *n;
	struct clk_hw *parent_hw;
	unsigned long flags;
	u8 id;
} sama5d3_systemck[] = {
	/*
	 * ddrck feeds DDR controller and is enabled by bootloader thus we need
	 * to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "ddrck", .id = 2, .flags = CLK_IS_CRITICAL },
	{ .n = "lcdck", .id = 3 },
	{ .n = "smdck", .id = 4 },
	{ .n = "uhpck", .id = 6 },
	{ .n = "udpck", .id = 7 },
	{ .n = "pck0",  .id = 8 },
	{ .n = "pck1",  .id = 9 },
	{ .n = "pck2",  .id = 10 },
};

static const struct {
	char *n;
	u8 id;
	struct clk_range r;
	unsigned long flags;
} sama5d3_periphck[] = {
	{ .n = "dbgu_clk", .id = 2, },
	{ .n = "hsmc_clk", .id = 5, },
	{ .n = "pioA_clk", .id = 6, },
	{ .n = "pioB_clk", .id = 7, },
	{ .n = "pioC_clk", .id = 8, },
	{ .n = "pioD_clk", .id = 9, },
	{ .n = "pioE_clk", .id = 10, },
	{ .n = "usart0_clk", .id = 12, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "usart1_clk", .id = 13, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "usart2_clk", .id = 14, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "usart3_clk", .id = 15, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "uart0_clk", .id = 16, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "uart1_clk", .id = 17, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "twi0_clk", .id = 18, .r = { .min = 0, .max = 41500000 }, },
	{ .n = "twi1_clk", .id = 19, .r = { .min = 0, .max = 41500000 }, },
	{ .n = "twi2_clk", .id = 20, .r = { .min = 0, .max = 41500000 }, },
	{ .n = "mci0_clk", .id = 21, },
	{ .n = "mci1_clk", .id = 22, },
	{ .n = "mci2_clk", .id = 23, },
	{ .n = "spi0_clk", .id = 24, .r = { .min = 0, .max = 166000000 }, },
	{ .n = "spi1_clk", .id = 25, .r = { .min = 0, .max = 166000000 }, },
	{ .n = "tcb0_clk", .id = 26, .r = { .min = 0, .max = 166000000 }, },
	{ .n = "tcb1_clk", .id = 27, .r = { .min = 0, .max = 166000000 }, },
	{ .n = "pwm_clk", .id = 28, },
	{ .n = "adc_clk", .id = 29, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "dma0_clk", .id = 30, },
	{ .n = "dma1_clk", .id = 31, },
	{ .n = "uhphs_clk", .id = 32, },
	{ .n = "udphs_clk", .id = 33, },
	{ .n = "macb0_clk", .id = 34, },
	{ .n = "macb1_clk", .id = 35, },
	{ .n = "lcdc_clk", .id = 36, },
	{ .n = "isi_clk", .id = 37, },
	{ .n = "ssc0_clk", .id = 38, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "ssc1_clk", .id = 39, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "can0_clk", .id = 40, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "can1_clk", .id = 41, .r = { .min = 0, .max = 83000000 }, },
	{ .n = "sha_clk", .id = 42, },
	{ .n = "aes_clk", .id = 43, },
	{ .n = "tdes_clk", .id = 44, },
	{ .n = "trng_clk", .id = 45, },
	{ .n = "fuse_clk", .id = 48, },
	/*
	 * mpddr_clk feeds DDR controller and is enabled by bootloader thus we
	 * need to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "mpddr_clk", .id = 49, .flags = CLK_IS_CRITICAL },
};

static void __init sama5d3_pmc_setup(struct device_node *np)
{
	const char *slow_clk_name = "slowck", *main_xtal_name = "main_xtal";
	struct clk_hw *main_rc_hw, *main_osc_hw, *mainck_hw;
	u8 slow_clk_index = 0, main_xtal_index = 1;
	struct clk_hw *smdck_hw, *usbck_hw, *hw;
	struct clk_parent_data parent_data[5];
	struct pmc_data *sama5d3_pmc;
	struct regmap *regmap;
	bool bypass;
	int i;

	regmap = device_node_to_regmap(np);
	if (IS_ERR(regmap))
		return;

	sama5d3_pmc = pmc_data_allocate(PMC_PLLACK + 1,
					nck(sama5d3_systemck),
					nck(sama5d3_periphck), 0, 3);
	if (!sama5d3_pmc)
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
	mainck_hw = at91_clk_register_sam9x5_main(regmap, "mainck", NULL, parent_data, 2);
	if (IS_ERR(mainck_hw))
		goto err_free;

	hw = at91_clk_register_pll(regmap, "pllack", NULL, &AT91_CLK_PD_HW(mainck_hw), 0,
				   &sama5d3_pll_layout, &plla_characteristics);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_plldiv(regmap, "plladivck", NULL, &AT91_CLK_PD_HW(hw));
	if (IS_ERR(hw))
		goto err_free;

	sama5d3_pmc->chws[PMC_PLLACK] = hw;

	hw = at91_clk_register_utmi(regmap, NULL, "utmick", NULL, &AT91_CLK_PD_HW(mainck_hw));
	if (IS_ERR(hw))
		goto err_free;

	sama5d3_pmc->chws[PMC_UTMI] = hw;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(mainck_hw);
	parent_data[2] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_UTMI]);
	hw = at91_clk_register_master_pres(regmap, "masterck_pres", 4,
					   NULL, parent_data,
					   &at91sam9x5_master_layout,
					   &mck_characteristics, &mck_lock);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_master_div(regmap, "masterck_div",
					  NULL, &AT91_CLK_PD_HW(hw),
					  &at91sam9x5_master_layout,
					  &mck_characteristics, &mck_lock,
					  CLK_SET_RATE_GATE, 0);
	if (IS_ERR(hw))
		goto err_free;

	sama5d3_pmc->chws[PMC_MCK] = hw;

	parent_data[0] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_UTMI]);
	usbck_hw = at91sam9x5_clk_register_usb(regmap, "usbck", NULL, parent_data, 2);
	if (IS_ERR(usbck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_UTMI]);
	smdck_hw = at91sam9x5_clk_register_smd(regmap, "smdclk", NULL, parent_data, 2);
	if (IS_ERR(smdck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(mainck_hw);
	parent_data[2] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_UTMI]);
	parent_data[4] = AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_MCK]);
	for (i = 0; i < 3; i++) {
		char name[6];

		snprintf(name, sizeof(name), "prog%d", i);

		hw = at91_clk_register_programmable(regmap, name,
						    NULL, parent_data, 5, i,
						    &at91sam9x5_programmable_layout,
						    NULL);
		if (IS_ERR(hw))
			goto err_free;

		sama5d3_pmc->pchws[i] = hw;
	}

	/* Set systemck parent hws. */
	sama5d3_systemck[0].parent_hw = sama5d3_pmc->chws[PMC_MCK];
	sama5d3_systemck[1].parent_hw = sama5d3_pmc->chws[PMC_MCK];
	sama5d3_systemck[2].parent_hw = smdck_hw;
	sama5d3_systemck[3].parent_hw = usbck_hw;
	sama5d3_systemck[4].parent_hw = usbck_hw;
	sama5d3_systemck[5].parent_hw = sama5d3_pmc->pchws[0];
	sama5d3_systemck[6].parent_hw = sama5d3_pmc->pchws[1];
	sama5d3_systemck[7].parent_hw = sama5d3_pmc->pchws[2];
	for (i = 0; i < ARRAY_SIZE(sama5d3_systemck); i++) {
		hw = at91_clk_register_system(regmap, sama5d3_systemck[i].n, NULL,
					      &AT91_CLK_PD_HW(sama5d3_systemck[i].parent_hw),
					      sama5d3_systemck[i].id,
					      sama5d3_systemck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		sama5d3_pmc->shws[sama5d3_systemck[i].id] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(sama5d3_periphck); i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &sama5d3_pcr_layout,
							 sama5d3_periphck[i].n,
							 NULL,
							 &AT91_CLK_PD_HW(sama5d3_pmc->chws[PMC_MCK]),
							 sama5d3_periphck[i].id,
							 &sama5d3_periphck[i].r,
							 INT_MIN,
							 sama5d3_periphck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		sama5d3_pmc->phws[sama5d3_periphck[i].id] = hw;
	}

	of_clk_add_hw_provider(np, of_clk_hw_pmc_get, sama5d3_pmc);

	return;

err_free:
	kfree(sama5d3_pmc);
}
/*
 * The TCB is used as the clocksource so its clock is needed early. This means
 * this can't be a platform driver.
 */
CLK_OF_DECLARE(sama5d3_pmc, "atmel,sama5d3-pmc", sama5d3_pmc_setup);
