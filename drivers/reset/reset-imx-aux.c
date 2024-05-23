// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

/*
 * The reset does not support the feature and corresponding
 * values are not valid
 */
#define ASSERT_NONE     BIT(0)
#define DEASSERT_NONE   BIT(1)
#define STATUS_NONE     BIT(2)

/* When set this function is activated by setting(vs clearing) this bit */
#define ASSERT_SET      BIT(3)
#define DEASSERT_SET    BIT(4)
#define STATUS_SET      BIT(5)

/* The following are the inverse of the above and are added for consistency */
#define ASSERT_CLEAR    (0 << 3)
#define DEASSERT_CLEAR  (0 << 4)
#define STATUS_CLEAR    (0 << 5)

/**
 * struct imx_reset_ctrl - reset control structure
 * @assert_offset: reset assert control register offset
 * @assert_bit: reset assert bit in the reset assert control register
 * @deassert_offset: reset deassert control register offset
 * @deassert_bit: reset deassert bit in the reset deassert control register
 * @status_offset: reset status register offset
 * @status_bit: reset status bit in the reset status register
 * @flags: reset flag indicating how the (de)assert and status are handled
 */
struct imx_reset_ctrl {
	u32 assert_offset;
	u32 assert_bit;
	u32 deassert_offset;
	u32 deassert_bit;
	u32 status_offset;
	u32 status_bit;
	u32 flags;
};

struct imx_reset_data {
	const struct imx_reset_ctrl *rst_ctrl;
	size_t rst_ctrl_num;
};

struct imx_aux_reset_priv {
	struct reset_controller_dev rcdev;
	void __iomem *base;
	const struct imx_reset_data *data;
};

static int imx_aux_reset_assert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct imx_aux_reset_priv *priv = container_of(rcdev,
					struct imx_aux_reset_priv, rcdev);
	const struct imx_reset_data *data = priv->data;
	void __iomem *reg_addr = priv->base;
	const struct imx_reset_ctrl *ctrl;
	unsigned int mask, value, reg;

	if (id >= data->rst_ctrl_num)
		return -EINVAL;

	ctrl = &data->rst_ctrl[id];

	/* assert not supported for this reset */
	if (ctrl->flags & ASSERT_NONE)
		return -EOPNOTSUPP;

	mask = BIT(ctrl->assert_bit);
	value = (ctrl->flags & ASSERT_SET) ? mask : 0x0;

	reg = readl(reg_addr + ctrl->assert_offset);
	writel(reg | value, reg_addr + ctrl->assert_offset);

	return 0;
}

static int imx_aux_reset_deassert(struct reset_controller_dev *rcdev,
				  unsigned long id)
{
	struct imx_aux_reset_priv *priv = container_of(rcdev,
					struct imx_aux_reset_priv, rcdev);
	const struct imx_reset_data *data = priv->data;
	void __iomem *reg_addr = priv->base;
	const struct imx_reset_ctrl *ctrl;
	unsigned int mask, value, reg;

	if (id >= data->rst_ctrl_num)
		return -EINVAL;

	ctrl = &data->rst_ctrl[id];

	/* deassert not supported for this reset */
	if (ctrl->flags & DEASSERT_NONE)
		return -EOPNOTSUPP;

	mask = BIT(ctrl->deassert_bit);
	value = (ctrl->flags & DEASSERT_SET) ? mask : 0x0;

	reg = readl(reg_addr + ctrl->deassert_offset);
	writel(reg | value, reg_addr + ctrl->deassert_offset);

	return 0;
}

static int imx_aux_reset_status(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct imx_aux_reset_priv *priv = container_of(rcdev,
					struct imx_aux_reset_priv, rcdev);
	const struct imx_reset_data *data = priv->data;
	void __iomem *reg_addr = priv->base;
	const struct imx_reset_ctrl *ctrl;
	unsigned int reset_state;

	if (id >= data->rst_ctrl_num)
		return -EINVAL;

	ctrl = &data->rst_ctrl[id];

	/* status not supported for this reset */
	if (ctrl->flags & STATUS_NONE)
		return -EOPNOTSUPP;

	reset_state = readl(reg_addr + ctrl->status_offset);

	return !(reset_state & BIT(ctrl->status_bit)) ==
		!(ctrl->flags & STATUS_SET);
}

static const struct reset_control_ops imx_aux_reset_ops = {
	.assert   = imx_aux_reset_assert,
	.deassert = imx_aux_reset_deassert,
	.status	  = imx_aux_reset_status,
};

static int imx_aux_reset_probe(struct auxiliary_device *adev,
			       const struct auxiliary_device_id *id)
{
	struct imx_reset_data *data = (struct imx_reset_data *)(id->driver_data);
	struct imx_aux_reset_priv *priv;
	struct device *dev = &adev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rcdev.owner     = THIS_MODULE;
	priv->rcdev.nr_resets = data->rst_ctrl_num;
	priv->rcdev.ops       = &imx_aux_reset_ops;
	priv->rcdev.of_node   = dev->parent->of_node;
	priv->rcdev.dev	      = dev;
	priv->rcdev.of_reset_n_cells = 1;
	priv->base            = of_iomap(dev->parent->of_node, 0);
	priv->data            = data;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

#define EARC  0x200

static const struct imx_reset_ctrl imx8mp_audiomix_rst_ctrl[] = {
	{
		.assert_offset = EARC,
		.assert_bit = 0,
		.deassert_offset = EARC,
		.deassert_bit = 0,
		.flags  = ASSERT_CLEAR | DEASSERT_SET | STATUS_NONE,
	},
	{
		.assert_offset = EARC,
		.assert_bit = 1,
		.deassert_offset = EARC,
		.deassert_bit = 1,
		.flags  = ASSERT_CLEAR | DEASSERT_SET | STATUS_NONE,
	},
};

static const struct imx_reset_data imx8mp_audiomix_rst_data = {
	.rst_ctrl = imx8mp_audiomix_rst_ctrl,
	.rst_ctrl_num = ARRAY_SIZE(imx8mp_audiomix_rst_ctrl),
};

static const struct auxiliary_device_id imx_aux_reset_ids[] = {
	{
		.name = "clk_imx8mp_audiomix.reset",
		.driver_data = (kernel_ulong_t)&imx8mp_audiomix_rst_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, imx_aux_reset_ids);

static struct auxiliary_driver imx_aux_reset_driver = {
	.probe		= imx_aux_reset_probe,
	.id_table	= imx_aux_reset_ids,
};

module_auxiliary_driver(imx_aux_reset_driver);

MODULE_AUTHOR("Shengjiu Wang <shengjiu.wang@nxp.com>");
MODULE_DESCRIPTION("Freescale i.MX auxiliary reset driver");
MODULE_LICENSE("GPL");
