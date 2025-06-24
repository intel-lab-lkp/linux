// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include <dt-bindings/clock/at91.h>

#include "pmc.h"

static DEFINE_SPINLOCK(rm9200_mck_lock);

struct sck {
	char *n;
	struct clk_hw *parent_hw;
	u8 id;
};

struct pck {
	char *n;
	u8 id;
};

static const struct clk_master_characteristics rm9200_mck_characteristics = {
	.output = { .min = 0, .max = 80000000 },
	.divisors = { 1, 2, 3, 4 },
};

static u8 rm9200_pll_out[] = { 0, 2 };

static const struct clk_range rm9200_pll_outputs[] = {
	{ .min = 80000000, .max = 160000000 },
	{ .min = 150000000, .max = 180000000 },
};

static const struct clk_pll_characteristics rm9200_pll_characteristics = {
	.input = { .min = 1000000, .max = 32000000 },
	.num_output = ARRAY_SIZE(rm9200_pll_outputs),
	.output = rm9200_pll_outputs,
	.out = rm9200_pll_out,
};

static struct sck at91rm9200_systemck[] = {
	{ .n = "udpck", .id = 1 },
	{ .n = "uhpck", .id = 4 },
	{ .n = "pck0",  .id = 8 },
	{ .n = "pck1",  .id = 9 },
	{ .n = "pck2",  .id = 10 },
	{ .n = "pck3",  .id = 11 },
};

static const struct pck at91rm9200_periphck[] = {
	{ .n = "pioA_clk",   .id = 2 },
	{ .n = "pioB_clk",   .id = 3 },
	{ .n = "pioC_clk",   .id = 4 },
	{ .n = "pioD_clk",   .id = 5 },
	{ .n = "usart0_clk", .id = 6 },
	{ .n = "usart1_clk", .id = 7 },
	{ .n = "usart2_clk", .id = 8 },
	{ .n = "usart3_clk", .id = 9 },
	{ .n = "mci0_clk",   .id = 10 },
	{ .n = "udc_clk",    .id = 11 },
	{ .n = "twi0_clk",   .id = 12 },
	{ .n = "spi0_clk",   .id = 13 },
	{ .n = "ssc0_clk",   .id = 14 },
	{ .n = "ssc1_clk",   .id = 15 },
	{ .n = "ssc2_clk",   .id = 16 },
	{ .n = "tc0_clk",    .id = 17 },
	{ .n = "tc1_clk",    .id = 18 },
	{ .n = "tc2_clk",    .id = 19 },
	{ .n = "tc3_clk",    .id = 20 },
	{ .n = "tc4_clk",    .id = 21 },
	{ .n = "tc5_clk",    .id = 22 },
	{ .n = "ohci_clk",   .id = 23 },
	{ .n = "macb0_clk",  .id = 24 },
};

static void __init at91rm9200_pmc_setup(struct device_node *np)
{
	const char *slow_clk_name = "slowck", *main_xtal_name = "main_xtal";
	struct clk_hw *usbck_hw, *main_osc_hw, *hw;
	u8 slow_clk_index = 0, main_xtal_index = 1;
	struct clk_parent_data parent_data[6];
	struct pmc_data *at91rm9200_pmc;
	u32 usb_div[] = { 1, 2, 0, 0 };
	struct regmap *regmap;
	bool bypass;
	int i;

	regmap = device_node_to_regmap(np);
	if (IS_ERR(regmap))
		return;

	at91rm9200_pmc = pmc_data_allocate(PMC_PLLBCK + 1,
					    nck(at91rm9200_systemck),
					    nck(at91rm9200_periphck), 0, 4);
	if (!at91rm9200_pmc)
		return;

	bypass = of_property_read_bool(np, "atmel,osc-bypass");

	main_osc_hw = at91_clk_register_main_osc(regmap, "main_osc", NULL,
						 &AT91_CLK_PD_NAME(main_xtal_name, main_xtal_index),
						 bypass);
	if (IS_ERR(main_osc_hw))
		goto err_free;

	hw = at91_clk_register_rm9200_main(regmap, "mainck", NULL,
					   &AT91_CLK_PD_HW(main_osc_hw));
	if (IS_ERR(hw))
		goto err_free;

	at91rm9200_pmc->chws[PMC_MAIN] = hw;

	hw = at91_clk_register_pll(regmap, "pllack", NULL,
				   &AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_MAIN]), 0,
				   &at91rm9200_pll_layout,
				   &rm9200_pll_characteristics);
	if (IS_ERR(hw))
		goto err_free;

	at91rm9200_pmc->chws[PMC_PLLACK] = hw;

	hw = at91_clk_register_pll(regmap, "pllbck", NULL,
				   &AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_MAIN]), 1,
				   &at91rm9200_pll_layout,
				   &rm9200_pll_characteristics);
	if (IS_ERR(hw))
		goto err_free;

	at91rm9200_pmc->chws[PMC_PLLBCK] = hw;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_MAIN]);
	parent_data[2] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_PLLBCK]);
	hw = at91_clk_register_master_pres(regmap, "masterck_pres", 4,
					   NULL, parent_data,
					   &at91rm9200_master_layout,
					   &rm9200_mck_characteristics,
					   &rm9200_mck_lock);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_master_div(regmap, "masterck_div", NULL, &AT91_CLK_PD_HW(hw),
					  &at91rm9200_master_layout,
					  &rm9200_mck_characteristics,
					  &rm9200_mck_lock, CLK_SET_RATE_GATE, 0);
	if (IS_ERR(hw))
		goto err_free;

	at91rm9200_pmc->chws[PMC_MCK] = hw;

	usbck_hw = at91rm9200_clk_register_usb(regmap, "usbck", NULL,
					       &AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_PLLBCK]),
					       usb_div);
	if (IS_ERR(usbck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_MAIN]);
	parent_data[2] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_PLLBCK]);
	for (i = 0; i < 4; i++) {
		char name[6];

		snprintf(name, sizeof(name), "prog%d", i);

		hw = at91_clk_register_programmable(regmap, name,
						    NULL, parent_data, 4, i,
						    &at91rm9200_programmable_layout,
						    NULL);
		if (IS_ERR(hw))
			goto err_free;

		at91rm9200_pmc->pchws[i] = hw;
	}

	/* Set systemck parent hws. */
	at91rm9200_systemck[0].parent_hw = usbck_hw;
	at91rm9200_systemck[1].parent_hw = usbck_hw;
	at91rm9200_systemck[2].parent_hw = at91rm9200_pmc->pchws[0];
	at91rm9200_systemck[3].parent_hw = at91rm9200_pmc->pchws[1];
	at91rm9200_systemck[4].parent_hw = at91rm9200_pmc->pchws[2];
	at91rm9200_systemck[5].parent_hw = at91rm9200_pmc->pchws[3];
	for (i = 0; i < ARRAY_SIZE(at91rm9200_systemck); i++) {
		hw = at91_clk_register_system(regmap, at91rm9200_systemck[i].n, NULL,
					      &AT91_CLK_PD_HW(at91rm9200_systemck[i].parent_hw),
					      at91rm9200_systemck[i].id, 0);
		if (IS_ERR(hw))
			goto err_free;

		at91rm9200_pmc->shws[at91rm9200_systemck[i].id] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(at91rm9200_periphck); i++) {
		hw = at91_clk_register_peripheral(regmap,
						  at91rm9200_periphck[i].n,
						  NULL,
						  &AT91_CLK_PD_HW(at91rm9200_pmc->chws[PMC_MCK]),
						  at91rm9200_periphck[i].id);
		if (IS_ERR(hw))
			goto err_free;

		at91rm9200_pmc->phws[at91rm9200_periphck[i].id] = hw;
	}

	of_clk_add_hw_provider(np, of_clk_hw_pmc_get, at91rm9200_pmc);

	return;

err_free:
	kfree(at91rm9200_pmc);
}
/*
 * While the TCB can be used as the clocksource, the system timer is most likely
 * to be used instead. However, the pinctrl driver doesn't support probe
 * deferring properly. Once this is fixed, this can be switched to a platform
 * driver.
 */
CLK_OF_DECLARE(at91rm9200_pmc, "atmel,at91rm9200-pmc", at91rm9200_pmc_setup);
