// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G3S SYSC reset driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/soc/renesas/rzg3s-sysc-reset.h>

#include <dt-bindings/reset/renesas,r9a08g045-sysc.h>

#define RZG3S_SYSC_USB_PWRRDY		0xd70
#define RZG3S_SYSC_PCIE_RST_RSM_B	0xd74
#define RZG3S_SYSC_RESET_MASK		0x1

/**
 * struct rzg3s_sysc_reset_info - SYSC reset information
 * @offset: offset to configure the reset
 * @assert_val: value to write to register on assert
 * @deassert_val: value to write to register on de-assert
 */
struct rzg3s_sysc_reset_info {
	u16 offset;
	u8 assert_val;
	u8 deassert_val;
};

/**
 * struct rzg3s_sysc_reset - SYSC reset
 * @info: SYSC reset information
 * @radev: SYSC reset auxiliary device
 * @rcdev: reset controller device
 */
struct rzg3s_sysc_reset {
	const struct rzg3s_sysc_reset_info *info;
	struct rzg3s_sysc_reset_adev *radev;
	struct reset_controller_dev rcdev;
};

#define to_rzg3s_sysc_reset(r)	container_of(r, struct rzg3s_sysc_reset, rcdev)

static int rzg3s_sysc_reset_set(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct rzg3s_sysc_reset *reset = to_rzg3s_sysc_reset(rcdev);
	struct rzg3s_sysc_reset_adev *radev = reset->radev;
	struct rzg3s_sysc_reset_info info = reset->info[id];
	unsigned long flags;
	u32 tmp;

	spin_lock_irqsave(radev->lock, flags);
	tmp = readl(radev->base + info.offset);
	tmp &= ~RZG3S_SYSC_RESET_MASK;
	tmp |= assert ? info.assert_val : info.deassert_val;
	writel(tmp, radev->base + info.offset);
	spin_unlock_irqrestore(radev->lock, flags);

	return 0;
}

static int rzg3s_sysc_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	return rzg3s_sysc_reset_set(rcdev, id, true);
}

static int rzg3s_sysc_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	return rzg3s_sysc_reset_set(rcdev, id, false);
}

static int rzg3s_sysc_reset_status(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	struct rzg3s_sysc_reset *reset = to_rzg3s_sysc_reset(rcdev);
	const struct rzg3s_sysc_reset_info info = reset->info[id];
	struct rzg3s_sysc_reset_adev *radev = reset->radev;
	u32 tmp;

	tmp = readl(radev->base + info.offset);
	tmp = !!(tmp & RZG3S_SYSC_RESET_MASK);

	return info.assert_val ? tmp : !tmp;
}

static const struct reset_control_ops rzg3s_sysc_reset_ops = {
	.assert = rzg3s_sysc_reset_assert,
	.deassert = rzg3s_sysc_reset_deassert,
	.status = rzg3s_sysc_reset_status,
};

static const struct rzg3s_sysc_reset_info rzg3s_sysc_reset_info[] = {
	[R9A08G045_SYSC_RESET_USB] = {
		.offset = RZG3S_SYSC_USB_PWRRDY, .assert_val = 1, .deassert_val = 0
	},
	[R9A08G045_SYSC_RESET_PCIE] = {
		.offset = RZG3S_SYSC_PCIE_RST_RSM_B, .assert_val = 0, .deassert_val = 1
	},
};

static int rzg3s_sysc_reset_probe(struct auxiliary_device *adev,
				  const struct auxiliary_device_id *id)
{
	struct rzg3s_sysc_reset_adev *reset_adev = to_rzg3s_sysc_reset_adev(adev);
	struct device *dev = &adev->dev;
	struct rzg3s_sysc_reset *reset;

	reset = devm_kzalloc(dev, sizeof(*reset), GFP_KERNEL);
	if (!reset)
		return -ENOMEM;

	reset->radev = reset_adev;
	reset->info = rzg3s_sysc_reset_info;

	reset->rcdev.ops = &rzg3s_sysc_reset_ops;
	reset->rcdev.of_reset_n_cells = 1;
	reset->rcdev.nr_resets = ARRAY_SIZE(rzg3s_sysc_reset_info);
	reset->rcdev.of_node = dev->parent->of_node;
	reset->rcdev.dev = dev;

	return devm_reset_controller_register(dev, &reset->rcdev);
}

static const struct auxiliary_device_id rzg3s_sysc_reset_ids[] = {
	{ .name = "rzg3s_sysc.reset" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, rzg3s_sysc_reset_ids);

static struct auxiliary_driver rzg3s_sysc_reset_driver = {
	.probe		= rzg3s_sysc_reset_probe,
	.id_table	= rzg3s_sysc_reset_ids,
};
module_auxiliary_driver(rzg3s_sysc_reset_driver);
