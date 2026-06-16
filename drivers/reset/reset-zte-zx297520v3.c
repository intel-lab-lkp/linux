// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */
#include <dt-bindings/clock/zte,zx297520v3-clk.h>
#include <linux/reset-controller.h>
#include <linux/platform_device.h>
#include <linux/auxiliary_bus.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/iopoll.h>
#include <linux/delay.h>

struct zte_reset_reg {
	u32 mask, wait_mask;
	u16 reg;
};

struct zte_reset_info {
	const struct zte_reset_reg *resets;
	unsigned int num;
};

struct zte_reset {
	struct reset_controller_dev rcdev;
	struct regmap *map;
	const struct zte_reset_reg *resets;
};

static inline struct zte_reset *to_zte_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct zte_reset, rcdev);
}

static int zx29_rst_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct zte_reset *rst = to_zte_reset(rcdev);

	return regmap_clear_bits(rst->map, rst->resets[id].reg, rst->resets[id].mask);
}

static int zx29_rst_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct zte_reset *rst = to_zte_reset(rcdev);
	int res;
	u32 val;

	res = regmap_set_bits(rst->map, rst->resets[id].reg, rst->resets[id].mask);
	if (res)
		return res;

	/* This is a special case used only by USB reset */
	if (rst->resets[id].wait_mask) {
		return regmap_read_poll_timeout(rst->map, rst->resets[id].reg + 4, val,
						val & rst->resets[id].wait_mask, 1, 100);
	}

	return 0;
}

static int zx29_rst_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct zte_reset *rst = to_zte_reset(rcdev);
	int res;

	res = regmap_test_bits(rst->map, rst->resets[id].reg, rst->resets[id].mask);
	if (res < 0)
		return res;

	return !res;
}

static const struct reset_control_ops zx29_rst_ops = {
	.assert		= zx29_rst_assert,
	.deassert	= zx29_rst_deassert,
	.status		= zx29_rst_status,
};

static const struct zte_reset_reg zx297520v3_top_resets[] = {
	/* This bit is set by ZTE's cpko.ko blob, it looks like a reset bit for the LTE DSP
	 * coprocessor. Clocks for it are in matrixclk.
	 */
	[ZX297520V3_ZSP_RESET]       = { .reg = 0x13c, .mask = BIT(0)            },

	[ZX297520V3_UART0_RESET]     = { .reg = 0x78,  .mask = BIT(6)  | BIT(7)  },
	[ZX297520V3_I2C0_RESET]      = { .reg = 0x74,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_RTC_RESET]       = { .reg = 0x74,  .mask = BIT(4)  | BIT(5)  },
	[ZX297520V3_TIMER_T08_RESET] = { .reg = 0x78,  .mask = BIT(4)  | BIT(5)  },
	[ZX297520V3_TIMER_T09_RESET] = { .reg = 0x78,  .mask = BIT(2)  | BIT(3)  },
	[ZX297520V3_PMM_RESET]       = { .reg = 0x74,  .mask = BIT(0)  | BIT(1)  },

	/* I haven't found any clocks for GPIO. It probably wouldn't make much
	 * sense anyway. Only one reset bit per controller.
	 */
	[ZX297520V3_GPIO_RESET]      = { .reg =  0x74, .mask = BIT(3)            },
	[ZX297520V3_GPIO8_RESET]     = { .reg =  0x74, .mask = BIT(2)            },

	[ZX297520V3_TIMER_T12_RESET] = { .reg =  0x74, .mask = BIT(6)  | BIT(7)  },
	[ZX297520V3_TIMER_T13_RESET] = { .reg =  0x7c, .mask = BIT(0)  | BIT(1)  },
	[ZX297520V3_TIMER_T14_RESET] = { .reg =  0x7c, .mask = BIT(2)  | BIT(3)  },
	[ZX297520V3_TIMER_T15_RESET] = { .reg =  0x74, .mask = BIT(10) | BIT(11) },
	[ZX297520V3_TIMER_T16_RESET] = { .reg =  0x7c, .mask = BIT(4)  | BIT(5)  },
	[ZX297520V3_TIMER_T17_RESET] = { .reg = 0x12c, .mask = BIT(0)  | BIT(1)  },
	[ZX297520V3_WDT_T18_RESET]   = { .reg =  0x74, .mask = BIT(12) | BIT(13) },
	[ZX297520V3_USIM1_RESET]     = { .reg =  0x74, .mask = BIT(14) | BIT(15) },
	[ZX297520V3_AHB_RESET]       = { .reg =  0x70, .mask = BIT(0)  | BIT(1)  },

	/* USB reset. This is slightly special because it needs to wait for a ready bit after
	 * deasserting.
	 */
	[ZX297520V3_USB_RESET]      =  { .reg = 0x80,   .mask = BIT(3) | BIT(4) | BIT(5),
		.wait_mask = BIT(1)},
	[ZX297520V3_HSIC_RESET]      = { .reg = 0x80,   .mask = BIT(0) | BIT(1) | BIT(2),
		.wait_mask = BIT(0)},
};

static const struct zte_reset_info zx297520v3_top_info = {
	.resets = zx297520v3_top_resets,
	.num = ARRAY_SIZE(zx297520v3_top_resets),
};

static const struct zte_reset_reg zx297520v3_matrix_resets[] = {
	[ZX297520V3_CPU_RESET]       = { .reg =  0x28, .mask = BIT(1)            },
	[ZX297520V3_EDCP_RESET]      = { .reg =  0x68, .mask = BIT(0)            },
	[ZX297520V3_SD0_RESET]       = { .reg =  0x58, .mask = BIT(1)            },
	[ZX297520V3_SD1_RESET]       = { .reg =  0x58, .mask = BIT(0)            },
	[ZX297520V3_NAND_RESET]      = { .reg =  0x58, .mask = BIT(4)            },
	[ZX297520V3_PDCFG_RESET]     = { .reg =  0x94, .mask = BIT(20)           },
	[ZX297520V3_SSC_RESET]       = { .reg =  0x94, .mask = BIT(24)           },
	[ZX297520V3_GMAC_RESET]      = { .reg = 0x114, .mask = BIT(0)  | BIT(1)  },
	[ZX297520V3_VOU_RESET]       = { .reg = 0x16c, .mask = BIT(0)            },
};

static const struct zte_reset_info zx297520v3_matrix_info = {
	.resets = zx297520v3_matrix_resets,
	.num = ARRAY_SIZE(zx297520v3_matrix_resets),
};

static const struct zte_reset_reg zx297520v3_lsp_resets[] = {
	[ZX297520V3_TIMER_L1_RESET]  = { .reg = 0x04,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_WDT_L2_RESET]    = { .reg = 0x08,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_WDT_L3_RESET]    = { .reg = 0x0c,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_PWM_RESET]       = { .reg = 0x10,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_I2S0_RESET]      = { .reg = 0x14,  .mask = BIT(8)  | BIT(9)  },
	/* 0x18: Not writeable */
	[ZX297520V3_I2S1_RESET]      = { .reg = 0x1c,  .mask = BIT(8)  | BIT(9)  },
	/* 0x20: Not writeable */
	[ZX297520V3_QSPI_RESET]      = { .reg = 0x24,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_UART1_RESET]     = { .reg = 0x28,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_I2C1_RESET]      = { .reg = 0x2c,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_SPI0_RESET]      = { .reg = 0x30,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_TIMER_LB_RESET]  = { .reg = 0x34,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_TIMER_LC_RESET]  = { .reg = 0x38,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_UART2_RESET]     = { .reg = 0x3c,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_WDT_LE_RESET]    = { .reg = 0x40,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_TIMER_LF_RESET]  = { .reg = 0x44,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_SPI1_RESET]      = { .reg = 0x48,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_TIMER_L11_RESET] = { .reg = 0x4c,  .mask = BIT(8)  | BIT(9)  },
	[ZX297520V3_TDM_RESET]       = { .reg = 0x50,  .mask = BIT(8)  | BIT(9)  },
};

static const struct zte_reset_info zx297520v3_lsp_info = {
	.resets = zx297520v3_lsp_resets,
	.num = ARRAY_SIZE(zx297520v3_lsp_resets),
};

static int reset_zx297520v3_probe(struct auxiliary_device *adev,
				  const struct auxiliary_device_id *id)
{
	const struct zte_reset_info *drv_info;
	struct device *dev = &adev->dev;
	struct zte_reset *rst;

	drv_info = (struct zte_reset_info *)id->driver_data;

	rst = devm_kzalloc(dev, sizeof(*rst), GFP_KERNEL);
	if (!rst)
		return -ENOMEM;

	rst->resets = drv_info->resets;
	rst->rcdev.owner = THIS_MODULE;
	rst->rcdev.nr_resets = drv_info->num;
	rst->rcdev.ops = &zx29_rst_ops;
	rst->rcdev.of_node = dev->of_node;
	rst->rcdev.dev = dev;
	rst->rcdev.of_reset_n_cells = 1;

	rst->map = device_node_to_regmap(dev->of_node);
	if (IS_ERR(rst->map))
		return dev_err_probe(dev, PTR_ERR(rst->map), "Cannot get parent syscon regmap\n");

	return devm_reset_controller_register(dev, &rst->rcdev);
}

static const struct auxiliary_device_id reset_zx297520v3_ids[] = {
	{
		.name = "clk_zte.zx297520v3_toprst",
		.driver_data = (kernel_ulong_t)&zx297520v3_top_info,
	},
	{
		.name = "clk_zte.zx297520v3_matrixrst",
		.driver_data = (kernel_ulong_t)&zx297520v3_matrix_info,
	},
	{
		.name = "clk_zte.zx297520v3_lsprst",
		.driver_data = (kernel_ulong_t)&zx297520v3_lsp_info,
	},
	{ },
};

MODULE_DEVICE_TABLE(auxiliary, reset_zx297520v3_ids);

static struct auxiliary_driver reset_zx297520v3_drv = {
	.name = "zx297520v3_reset",
	.id_table = reset_zx297520v3_ids,
	.probe = reset_zx297520v3_probe,
};

module_auxiliary_driver(reset_zx297520v3_drv);

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE zx297520v3 reset driver");
MODULE_LICENSE("GPL");
