// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include <dt-bindings/clock/at91.h>

#include "pmc.h"

static DEFINE_SPINLOCK(mck_lock);

static const struct clk_master_characteristics mck_characteristics = {
	.output = { .min = 125000000, .max = 200000000 },
	.divisors = { 1, 2, 4, 3 },
};

static u8 plla_out[] = { 0 };

static u16 plla_icpll[] = { 0 };

static const struct clk_range plla_outputs[] = {
	{ .min = 600000000, .max = 1200000000 },
};

static const struct clk_pll_characteristics plla_characteristics = {
	.input = { .min = 12000000, .max = 12000000 },
	.num_output = ARRAY_SIZE(plla_outputs),
	.output = plla_outputs,
	.icpll = plla_icpll,
	.out = plla_out,
};

static const struct clk_pcr_layout sama5d4_pcr_layout = {
	.offset = 0x10c,
	.cmd = BIT(12),
	.pid_mask = GENMASK(6, 0),
};

static struct {
	char *n;
	struct clk_hw *parent_hw;
	unsigned long flags;
	u8 id;
} sama5d4_systemck[] = {
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
} sama5d4_periph32ck[] = {
	{ .n = "pioD_clk", .id = 5 },
	{ .n = "usart0_clk", .id = 6 },
	{ .n = "usart1_clk", .id = 7 },
	{ .n = "icm_clk", .id = 9 },
	{ .n = "aes_clk", .id = 12 },
	{ .n = "tdes_clk", .id = 14 },
	{ .n = "sha_clk", .id = 15 },
	{ .n = "matrix1_clk", .id = 17 },
	{ .n = "hsmc_clk", .id = 22 },
	{ .n = "pioA_clk", .id = 23 },
	{ .n = "pioB_clk", .id = 24 },
	{ .n = "pioC_clk", .id = 25 },
	{ .n = "pioE_clk", .id = 26 },
	{ .n = "uart0_clk", .id = 27 },
	{ .n = "uart1_clk", .id = 28 },
	{ .n = "usart2_clk", .id = 29 },
	{ .n = "usart3_clk", .id = 30 },
	{ .n = "usart4_clk", .id = 31 },
	{ .n = "twi0_clk", .id = 32 },
	{ .n = "twi1_clk", .id = 33 },
	{ .n = "twi2_clk", .id = 34 },
	{ .n = "mci0_clk", .id = 35 },
	{ .n = "mci1_clk", .id = 36 },
	{ .n = "spi0_clk", .id = 37 },
	{ .n = "spi1_clk", .id = 38 },
	{ .n = "spi2_clk", .id = 39 },
	{ .n = "tcb0_clk", .id = 40 },
	{ .n = "tcb1_clk", .id = 41 },
	{ .n = "tcb2_clk", .id = 42 },
	{ .n = "pwm_clk", .id = 43 },
	{ .n = "adc_clk", .id = 44 },
	{ .n = "dbgu_clk", .id = 45 },
	{ .n = "uhphs_clk", .id = 46 },
	{ .n = "udphs_clk", .id = 47 },
	{ .n = "ssc0_clk", .id = 48 },
	{ .n = "ssc1_clk", .id = 49 },
	{ .n = "trng_clk", .id = 53 },
	{ .n = "macb0_clk", .id = 54 },
	{ .n = "macb1_clk", .id = 55 },
	{ .n = "fuse_clk", .id = 57 },
	{ .n = "securam_clk", .id = 59 },
	{ .n = "smd_clk", .id = 61 },
	{ .n = "twi3_clk", .id = 62 },
	{ .n = "catb_clk", .id = 63 },
};

static const struct {
	char *n;
	unsigned long flags;
	u8 id;
} sama5d4_periphck[] = {
	{ .n = "dma0_clk", .id = 8 },
	{ .n = "cpkcc_clk", .id = 10 },
	{ .n = "aesb_clk", .id = 13 },
	/*
	 * mpddr_clk feeds DDR controller and is enabled by bootloader thus we
	 * need to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "mpddr_clk", .id = 16, .flags = CLK_IS_CRITICAL },
	{ .n = "matrix0_clk", .id = 18 },
	{ .n = "vdec_clk", .id = 19 },
	{ .n = "dma1_clk", .id = 50 },
	{ .n = "lcdc_clk", .id = 51 },
	{ .n = "isi_clk", .id = 52 },
};

static void __init sama5d4_pmc_setup(struct device_node *np)
{
	const char *slow_clk_name = "slowck", *main_xtal_name = "main_xtal";
	struct clk_hw *main_rc_hw, *main_osc_hw, *mainck_hw;
	u8 slow_clk_index = 0, main_xtal_index = 1;
	struct clk_hw *smdck_hw, *usbck_hw, *hw;
	struct clk_range range = CLK_RANGE(0, 0);
	struct clk_parent_data parent_data[5];
	struct pmc_data *sama5d4_pmc;
	struct regmap *regmap;
	bool bypass;
	int i;

	regmap = device_node_to_regmap(np);
	if (IS_ERR(regmap))
		return;

	sama5d4_pmc = pmc_data_allocate(PMC_PLLACK + 1,
					nck(sama5d4_systemck),
					nck(sama5d4_periph32ck), 0, 3);
	if (!sama5d4_pmc)
		return;

	main_rc_hw = at91_clk_register_main_rc_osc(regmap, "main_rc_osc", 12000000,
						   100000000);
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
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_pll(regmap, "pllack", NULL, &AT91_CLK_PD_HW(mainck_hw), 0,
				   &sama5d3_pll_layout, &plla_characteristics);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_plldiv(regmap, "plladivck", NULL, &AT91_CLK_PD_HW(hw));
	if (IS_ERR(hw))
		goto err_free;

	sama5d4_pmc->chws[PMC_PLLACK] = hw;

	hw = at91_clk_register_utmi(regmap, NULL, "utmick", NULL, &AT91_CLK_PD_HW(mainck_hw));
	if (IS_ERR(hw))
		goto err_free;

	sama5d4_pmc->chws[PMC_UTMI] = hw;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(mainck_hw);
	parent_data[2] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_UTMI]);
	hw = at91_clk_register_master_pres(regmap, "masterck_pres", 4,
					   NULL, parent_data,
					   &at91sam9x5_master_layout,
					   &mck_characteristics, &mck_lock);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_master_div(regmap, "masterck_div", NULL, &AT91_CLK_PD_HW(hw),
					  &at91sam9x5_master_layout,
					  &mck_characteristics, &mck_lock,
					  CLK_SET_RATE_GATE, 0);
	if (IS_ERR(hw))
		goto err_free;

	sama5d4_pmc->chws[PMC_MCK] = hw;

	hw = at91_clk_register_h32mx(regmap, "h32mxck", NULL,
				     &AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_MCK]));
	if (IS_ERR(hw))
		goto err_free;

	sama5d4_pmc->chws[PMC_MCK2] = hw;

	parent_data[0] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_UTMI]);
	usbck_hw = at91sam9x5_clk_register_usb(regmap, "usbck", NULL, parent_data, 2);
	if (IS_ERR(usbck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_PLLACK]);
	parent_data[1] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_UTMI]);
	smdck_hw = at91sam9x5_clk_register_smd(regmap, "smdclk", NULL, parent_data, 2);
	if (IS_ERR(smdck_hw))
		goto err_free;

	parent_data[0] = AT91_CLK_PD_NAME(slow_clk_name, slow_clk_index);
	parent_data[1] = AT91_CLK_PD_HW(mainck_hw);
	parent_data[2] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_PLLACK]);
	parent_data[3] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_UTMI]);
	parent_data[4] = AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_MCK]);
	for (i = 0; i < 3; i++) {
		char name[6];

		snprintf(name, sizeof(name), "prog%d", i);

		hw = at91_clk_register_programmable(regmap, name,
						    NULL, parent_data, 5, i,
						    &at91sam9x5_programmable_layout,
						    NULL);
		if (IS_ERR(hw))
			goto err_free;

		sama5d4_pmc->pchws[i] = hw;
	}

	/* Set systemck parent hws. */
	sama5d4_systemck[0].parent_hw = sama5d4_pmc->chws[PMC_MCK];
	sama5d4_systemck[1].parent_hw = sama5d4_pmc->chws[PMC_MCK];
	sama5d4_systemck[2].parent_hw = smdck_hw;
	sama5d4_systemck[3].parent_hw = usbck_hw;
	sama5d4_systemck[4].parent_hw = usbck_hw;
	sama5d4_systemck[5].parent_hw = sama5d4_pmc->pchws[0];
	sama5d4_systemck[6].parent_hw = sama5d4_pmc->pchws[1];
	sama5d4_systemck[7].parent_hw = sama5d4_pmc->pchws[2];
	for (i = 0; i < ARRAY_SIZE(sama5d4_systemck); i++) {
		hw = at91_clk_register_system(regmap, sama5d4_systemck[i].n, NULL,
					      &AT91_CLK_PD_HW(sama5d4_systemck[i].parent_hw),
					      sama5d4_systemck[i].id,
					      sama5d4_systemck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		sama5d4_pmc->shws[sama5d4_systemck[i].id] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(sama5d4_periphck); i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &sama5d4_pcr_layout,
							 sama5d4_periphck[i].n,
							 NULL,
							 &AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_MCK]),
							 sama5d4_periphck[i].id,
							 &range, INT_MIN,
							 sama5d4_periphck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		sama5d4_pmc->phws[sama5d4_periphck[i].id] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(sama5d4_periph32ck); i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &sama5d4_pcr_layout,
							 sama5d4_periph32ck[i].n,
							 NULL,
							 &AT91_CLK_PD_HW(sama5d4_pmc->chws[PMC_MCK2]),
							 sama5d4_periph32ck[i].id,
							 &range, INT_MIN, 0);
		if (IS_ERR(hw))
			goto err_free;

		sama5d4_pmc->phws[sama5d4_periph32ck[i].id] = hw;
	}

	of_clk_add_hw_provider(np, of_clk_hw_pmc_get, sama5d4_pmc);

	return;

err_free:
	kfree(sama5d4_pmc);
}

CLK_OF_DECLARE(sama5d4_pmc, "atmel,sama5d4-pmc", sama5d4_pmc_setup);
