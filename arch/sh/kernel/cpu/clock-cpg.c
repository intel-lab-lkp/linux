// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clkdev.h>
#include <asm/clock.h>

static struct clk master_clk = {
	.flags		= CLK_ENABLE_ON_INIT,
	.rate		= CONFIG_SH_PCLK_FREQ,
};

static struct clk peripheral_clk = {
	.parent		= &master_clk,
	.flags		= CLK_ENABLE_ON_INIT,
};

#ifdef CONFIG_CPU_SUBTYPE_SH7305
/*
 * SH7305 TMU0 module clock.
 *
 * MSTPCR0 bit 15 gates the peripheral clock to TMU0.
 * Early platform initialization enables the gate before timer setup.
 */
static struct clk sh7305_tmu0_clk =
	SH_CLK_MSTP32(&peripheral_clk, 0xa4150030, 15, 0);

static struct clk_lookup sh7305_tmu0_lookup =
	CLKDEV_ICK_ID("fck", "sh-tmu.0", &sh7305_tmu0_clk);
#endif

static struct clk bus_clk = {
	.parent		= &master_clk,
	.flags		= CLK_ENABLE_ON_INIT,
};

static struct clk cpu_clk = {
	.parent		= &master_clk,
	.flags		= CLK_ENABLE_ON_INIT,
};

/*
 * The ordering of these clocks matters, do not change it.
 */
static struct clk *onchip_clocks[] = {
	&master_clk,
	&peripheral_clk,
	&bus_clk,
	&cpu_clk,
};

static struct clk_lookup lookups[] = {
	/* main clocks */
	CLKDEV_CON_ID("master_clk", &master_clk),
	CLKDEV_CON_ID("peripheral_clk", &peripheral_clk),
	CLKDEV_CON_ID("bus_clk", &bus_clk),
	CLKDEV_CON_ID("cpu_clk", &cpu_clk),
};

int __init __deprecated cpg_clk_init(void)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(onchip_clocks); i++) {
		struct clk *clk = onchip_clocks[i];
		arch_init_clk_ops(&clk->ops, i);
		if (clk->ops)
			ret |= clk_register(clk);
	}

	clkdev_add_table(lookups, ARRAY_SIZE(lookups));

	clk_add_alias("fck", "sh-tmu-sh3.0", "peripheral_clk", NULL);
#ifdef CONFIG_CPU_SUBTYPE_SH7305
	if (!ret) {
		ret = sh_clk_mstp_register(&sh7305_tmu0_clk, 1);
		if (!ret)
			clkdev_add(&sh7305_tmu0_lookup);
	}
#else
	clk_add_alias("fck", "sh-tmu.0", "peripheral_clk", NULL);
#endif
	clk_add_alias("fck", "sh-tmu.1", "peripheral_clk", NULL);
	clk_add_alias("fck", "sh-tmu.2", "peripheral_clk", NULL);
	clk_add_alias("fck", "sh-mtu2", "peripheral_clk", NULL);
	clk_add_alias("fck", "sh-cmt-16.0", "peripheral_clk", NULL);
	clk_add_alias("fck", "sh-cmt-32.0", "peripheral_clk", NULL);

	return ret;
}

/*
 * Placeholder for compatibility, until the lazy CPUs do this
 * on their own.
 */
int __init __weak arch_clk_init(void)
{
	return cpg_clk_init();
}
