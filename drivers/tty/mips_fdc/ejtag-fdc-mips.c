// SPDX-License-Identifier: GPL-2.0

#include "ejtag-fdc.h"

void __iomem *get_fdc_regs(void)
{
	return mips_cdmm_early_probe(0xfd);
}

int is_fdc_interrupt(void)
{
	return read_c0_cause() & CAUSEF_FDCI;
}

static const struct mips_cdmm_device_id mips_ejtag_fdc_tty_ids[] = {
	{ .type = 0xfd },
	{ }
};

static struct mips_cdmm_driver mips_ejtag_fdc_tty_driver = {
	.drv		= {
		.name	= "mips_ejtag_fdc",
	},
	.probe		= mips_ejtag_fdc_tty_probe,
	.cpu_down	= mips_ejtag_fdc_tty_cpu_down,
	.cpu_up		= mips_ejtag_fdc_tty_cpu_up,
	.id_table	= mips_ejtag_fdc_tty_ids,
};
builtin_mips_cdmm_driver(mips_ejtag_fdc_tty_driver);
