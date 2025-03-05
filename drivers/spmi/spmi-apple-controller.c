// SPDX-License-Identifier: GPL-2.0
/*
 * Apple SoC SPMI device driver
 *
 * Copyright The Asahi Linux Contributors
 *
 * Inspired by:
 *		OpenBSD support Copyright (c) 2021 Mark Kettenis <kettenis@openbsd.org>
 *		Correllium support Copyright (C) 2021 Corellium LLC
 *		hisi-spmi-controller.c
 *		spmi-pmic-ard.c Copyright (c) 2021, The Linux Foundation.
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spmi.h>

/* SPMI Controller Registers */
#define SPMI_STATUS_REG 0
#define SPMI_CMD_REG 0x4
#define SPMI_RSP_REG 0x8

#define SPMI_RX_FIFO_EMPTY BIT(24)

#define REG_POLL_INTERVAL 10000
#define REG_POLL_TIMEOUT (REG_POLL_INTERVAL * 5)

struct apple_spmi {
	void __iomem *regs;
};

#define poll_reg(spmi, reg, val, cond) \
	readl_relaxed_poll_timeout((spmi)->regs + (reg), (val), (cond), \
				   REG_POLL_INTERVAL, REG_POLL_TIMEOUT)

static inline u32 read_reg(struct apple_spmi *spmi, int offset)
{
	return readl_relaxed(spmi->regs + offset);
}

static inline void write_reg(u32 value, struct apple_spmi *spmi, int offset)
{
	writel_relaxed(value, spmi->regs + offset);
}

static int spmi_read_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			 u16 saddr, u8 *__buf, size_t bc)
{
	struct apple_spmi *spmi = spmi_controller_get_drvdata(ctrl);
	u32 spmi_cmd = opc | sid << 8 | saddr << 16 | (bc - 1) | (1 << 15);
	u32 rsp;
	u32 status;
	size_t len_to_read = 0;
	u8 i;
	int ret;

	write_reg(spmi_cmd, spmi, SPMI_CMD_REG);

	/* Wait for Rx FIFO to have something */
	ret = poll_reg(spmi, SPMI_STATUS_REG, status, !(status & SPMI_RX_FIFO_EMPTY));
	if (ret) {
		dev_err(&ctrl->dev,
			"%s:Failed to wait for RX FIFO not empty\n", __func__);
		return ret;
	}

	/* Discard SPMI reply status */
	read_reg(spmi, SPMI_RSP_REG);

	/* Read SPMI data reply */
	while (len_to_read < bc) {
		rsp = read_reg(spmi, SPMI_RSP_REG);
		i = 0;
		while ((len_to_read < bc) && (i < 4)) {
			__buf[len_to_read++] = ((0xff << (8 * i)) & rsp) >>
					       (8 * i);
			i += 1;
		}
	}

	return 0;
}

static int spmi_write_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			  u16 saddr, const u8 *__buf, size_t bc)
{
	struct apple_spmi *spmi = spmi_controller_get_drvdata(ctrl);
	u32 spmi_cmd = opc | sid << 8 | saddr << 16 | (bc - 1) | (1 << 15);
	u32 status;
	size_t i = 0, j;
	int ret;

	write_reg(spmi_cmd, spmi, SPMI_CMD_REG);

	while (i < bc) {
		j = 0;
		spmi_cmd = 0;
		while ((j < 4) & (i < bc))
			spmi_cmd |= __buf[i++] << (j++ * 8);

		write_reg(spmi_cmd, spmi, SPMI_CMD_REG);
	}

	/* Wait for Rx FIFO to have something */
	ret = poll_reg(spmi, SPMI_STATUS_REG, status, !(status & SPMI_RX_FIFO_EMPTY));
	if (ret) {
		dev_err(&ctrl->dev,
			"%s:Failed to wait for RX FIFO not empty\n", __func__);
		return ret;
	}

	/* Discard */
	read_reg(spmi, SPMI_RSP_REG);

	return 0;
}

static int spmi_controller_probe(struct platform_device *pdev)
{
	struct apple_spmi *spmi;
	struct spmi_controller *ctrl;
	int ret;

	ctrl = devm_spmi_controller_alloc(&pdev->dev, sizeof(*spmi));
	if (IS_ERR(ctrl)) {
		dev_err_probe(&pdev->dev, PTR_ERR(ctrl),
			      "Can't allocate spmi_controller data\n");
		return -ENOMEM;
	}

	spmi = spmi_controller_get_drvdata(ctrl);

	spmi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spmi->regs)) {
		dev_err_probe(&pdev->dev, PTR_ERR(spmi->regs),
			      "Can't get ioremap regs\n");
		return PTR_ERR(spmi->regs);
	}

	ctrl->dev.of_node = of_node_get(pdev->dev.of_node);

	ctrl->read_cmd = spmi_read_cmd;
	ctrl->write_cmd = spmi_write_cmd;

	ret = devm_spmi_controller_add(&pdev->dev, ctrl);
	if (ret) {
		dev_err(&pdev->dev,
			"spmi_controller_add failed with error %d!\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id spmi_controller_match_table[] = {
	{ .compatible = "apple,spmi", },
	{}
};
MODULE_DEVICE_TABLE(of, spmi_controller_match_table);

static struct platform_driver spmi_controller_driver = {
	.probe		= spmi_controller_probe,
	.driver		= {
		.name	= "apple-spmi",
		.of_match_table = spmi_controller_match_table,
	},
};
module_platform_driver(spmi_controller_driver);

MODULE_AUTHOR("Jean-Francois Bortolotti <jeff@borto.fr>");
MODULE_DESCRIPTION("Apple SoC SPMI driver");
MODULE_LICENSE("GPL");
