// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas SH7305 interrupt controller support
 *
 * Copyright (C) 2026 Artem Novak
 */

#include <linux/init.h>
#include <linux/sh_intc.h>

enum {
	UNUSED = 0,
	ENABLED,
	DISABLED,

	TMU0_TUNI0,
	TMU0_TUNI1,
	TMU0_TUNI2,
};

/*
 * SH7305 TMU interrupt event codes.
 */
static struct intc_vect vectors[] __initdata = {
	INTC_VECT(TMU0_TUNI0, 0x400),
	INTC_VECT(TMU0_TUNI1, 0x420),
	INTC_VECT(TMU0_TUNI2, 0x440),
};

/*
 * SH7305 IMR4 / IMCR4
 *
 * TMU0 channels occupy bits corresponding to TUNI0..2.
 */
static struct intc_mask_reg mask_registers[] __initdata = {
	{
		0xa4080090, 0xa40800d0, 8,
		{
			0,
			TMU0_TUNI2,
			TMU0_TUNI1,
			TMU0_TUNI0,
			0,
			0,
			0,
			0,
		}
	},
};

/*
 * SH7305 IPRA
 *
 * Four 4-bit interrupt priority fields:
 *
 *   bits 15..12  TMU0 channel 0
 *   bits 11..8   TMU0 channel 1
 *   bits  7..4   TMU0 channel 2
 *   bits  3..0   unused here
 */
static struct intc_prio_reg prio_registers[] __initdata = {
	{
		0xa4080000, 0, 16, 4,
		{
			TMU0_TUNI0,
			TMU0_TUNI1,
			TMU0_TUNI2,
			0,
		}
	},
};

static struct intc_desc intc_desc __initdata = {
	.name		= "sh7305",
	.force_enable	= ENABLED,
	.force_disable	= DISABLED,

	.hw = INTC_HW_DESC(vectors, NULL, mask_registers,
			     prio_registers, NULL, NULL),
};

void __init plat_irq_setup(void)
{
	register_intc_controller(&intc_desc);
}
