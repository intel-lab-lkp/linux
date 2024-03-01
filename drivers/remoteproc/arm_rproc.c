// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * Authors:
 *   Abdellatif El Khlifi <abdellatif.elkhlifi@arm.com>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include "remoteproc_internal.h"

/**
 * struct arm_rproc_reset_cfg - remote processor reset configuration
 * @ctrl_reg: address of the control register
 * @state_reg: address of the reset status register
 */
struct arm_rproc_reset_cfg {
	void __iomem *ctrl_reg;
	void __iomem *state_reg;
};

struct arm_rproc;

/**
 * struct arm_rproc_dcfg - Arm remote processor configuration
 * @stop: stop callback function
 * @start: start callback function
 */
struct arm_rproc_dcfg {
	int (*stop)(struct rproc *rproc);
	int (*start)(struct rproc *rproc);
};

/**
 * struct arm_rproc - Arm remote processor instance
 * @rproc: rproc handler
 * @core_dcfg: device configuration pointer
 * @reset_cfg: reset configuration registers
 */
struct arm_rproc {
	struct rproc				*rproc;
	const struct arm_rproc_dcfg		*core_dcfg;
	struct arm_rproc_reset_cfg		reset_cfg;
};

/* Definitions for Arm Corstone-1000 External System */

#define EXTSYS_RST_CTRL_CPUWAIT			BIT(0)
#define EXTSYS_RST_CTRL_RST_REQ			BIT(1)

#define EXTSYS_RST_ACK_MASK				GENMASK(2, 1)
#define EXTSYS_RST_ST_RST_ACK(x)			\
				((u8)(FIELD_GET(EXTSYS_RST_ACK_MASK, (x))))

#define EXTSYS_RST_ACK_NO_RESET_REQ			(0x0)
#define EXTSYS_RST_ACK_NOT_COMPLETE			(0x1)
#define EXTSYS_RST_ACK_COMPLETE			(0x2)
#define EXTSYS_RST_ACK_RESERVED			(0x3)

#define EXTSYS_RST_ACK_POLL_TRIES			(3)
#define EXTSYS_RST_ACK_POLL_TIMEOUT			(1000)

/**
 * arm_rproc_start_cs1000_extsys() - custom start function
 * @rproc: pointer to the remote processor object
 *
 * Start function for Corstone-1000 External System.
 * Allow the External System core start execute instructions.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_start_cs1000_extsys(struct rproc *rproc)
{
	struct arm_rproc *priv = rproc->priv;
	u32 ctrl_reg;

	/* CPUWAIT signal of the External System is de-asserted */
	ctrl_reg = readl(priv->reset_cfg.ctrl_reg);
	ctrl_reg &= ~EXTSYS_RST_CTRL_CPUWAIT;
	writel(ctrl_reg, priv->reset_cfg.ctrl_reg);

	return 0;
}

/**
 * arm_rproc_cs1000_extsys_poll_rst_ack() - poll RST_ACK bits
 * @rproc: pointer to the remote processor object
 * @exp_ack: expected bits value
 * @rst_ack: bits value read
 *
 * Tries to read RST_ACK bits until the timeout expires.
 * EXTSYS_RST_ACK_POLL_TRIES tries are made,
 * every EXTSYS_RST_ACK_POLL_TIMEOUT milliseconds.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_cs1000_extsys_poll_rst_ack(struct rproc *rproc,
						u8 exp_ack, u8 *rst_ack)
{
	struct arm_rproc *priv = rproc->priv;
	struct device *dev = rproc->dev.parent;
	u32 state_reg;
	int tries = EXTSYS_RST_ACK_POLL_TRIES;
	unsigned long timeout;

	do {
		state_reg = readl(priv->reset_cfg.state_reg);
		*rst_ack = EXTSYS_RST_ST_RST_ACK(state_reg);

		if (*rst_ack == EXTSYS_RST_ACK_RESERVED) {
			dev_err(dev, "unexpected RST_ACK value: 0x%x\n",
				*rst_ack);
			return -EINVAL;
		}

		/* expected ACK value read */
		if ((*rst_ack & exp_ack) || (*rst_ack == exp_ack))
			return 0;

		timeout = msleep_interruptible(EXTSYS_RST_ACK_POLL_TIMEOUT);

		if (timeout) {
			dev_err(dev, "polling RST_ACK  aborted\n");
			return -ECONNABORTED;
		}
	} while (--tries);

	dev_err(dev, "polling RST_ACK timed out\n");

	return -ETIMEDOUT;
}

/**
 * arm_rproc_stop_cs1000_extsys() - custom stop function
 * @rproc: pointer to the remote processor object
 *
 * Reset all logic within the External System, the core will be in a halt state.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_stop_cs1000_extsys(struct rproc *rproc)
{
	struct arm_rproc *priv = rproc->priv;
	struct device *dev = rproc->dev.parent;
	u32 ctrl_reg;
	u8 rst_ack, req_status;
	int ret;

	ctrl_reg = readl(priv->reset_cfg.ctrl_reg);
	ctrl_reg |= EXTSYS_RST_CTRL_RST_REQ;
	writel(ctrl_reg, priv->reset_cfg.ctrl_reg);

	ret = arm_rproc_cs1000_extsys_poll_rst_ack(rproc,
						   EXTSYS_RST_ACK_COMPLETE |
						   EXTSYS_RST_ACK_NOT_COMPLETE,
						   &rst_ack);
	if (ret)
		return ret;

	req_status = rst_ack;

	ctrl_reg = readl(priv->reset_cfg.ctrl_reg);
	ctrl_reg &= ~EXTSYS_RST_CTRL_RST_REQ;
	writel(ctrl_reg, priv->reset_cfg.ctrl_reg);

	ret = arm_rproc_cs1000_extsys_poll_rst_ack(rproc, 0, &rst_ack);
	if (ret)
		return ret;

	if (req_status == EXTSYS_RST_ACK_COMPLETE) {
		dev_dbg(dev, "the requested reset has been accepted\n");
		return 0;
	}

	dev_err(dev, "the requested reset has been denied\n");
	return -EACCES;
}

static const struct arm_rproc_dcfg arm_rproc_cfg_corstone1000_extsys = {
	.stop          = arm_rproc_stop_cs1000_extsys,
	.start         = arm_rproc_start_cs1000_extsys,
};

/**
 * arm_rproc_stop() - Stop function for rproc_ops
 * @rproc: pointer to the remote processor object
 *
 * Calls the stop() callback of the remote core
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_stop(struct rproc *rproc)
{
	struct arm_rproc *priv = rproc->priv;

	return priv->core_dcfg->stop(rproc);
}

/**
 * arm_rproc_start() - Start function for rproc_ops
 * @rproc: pointer to the remote processor object
 *
 * Calls the start() callback of the remote core
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_start(struct rproc *rproc)
{
	struct arm_rproc *priv = rproc->priv;

	return priv->core_dcfg->start(rproc);
}

/**
 * arm_rproc_parse_fw() - Parse firmware function for rproc_ops
 * @rproc: pointer to the remote processor object
 * @fw: pointer to the firmware
 *
 * Does nothing currently.
 *
 * Return:
 *
 * 0 for success.
 */
static int arm_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	return 0;
}

/**
 * arm_rproc_load() - Load firmware to memory function for rproc_ops
 * @rproc: pointer to the remote processor object
 * @fw: pointer to the firmware
 *
 * Does nothing currently.
 *
 * Return:
 *
 * 0 for success.
 */
static int arm_rproc_load(struct rproc *rproc, const struct firmware *fw)
{
	return 0;
}

static const struct rproc_ops arm_rproc_ops = {
	.start		= arm_rproc_start,
	.stop		= arm_rproc_stop,
	.load		= arm_rproc_load,
	.parse_fw	= arm_rproc_parse_fw,
};

/**
 * arm_rproc_probe() - the platform device probe
 * @pdev: the platform device
 *
 * Read from the device tree the properties needed to setup
 * the reset and comms for the remote processor.
 * Also, allocate a rproc device and register it with the remoteproc subsystem.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int arm_rproc_probe(struct platform_device *pdev)
{
	const struct arm_rproc_dcfg *core_dcfg;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct arm_rproc *priv;
	struct rproc *rproc;
	const char *fw_name;
	int ret;
	struct resource *res;

	core_dcfg = of_device_get_match_data(dev);
	if (!core_dcfg)
		return -ENODEV;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret) {
		dev_err(dev,
			"can't parse firmware-name from device tree (%pe)\n",
			ERR_PTR(ret));
		return ret;
	}

	dev_dbg(dev, "firmware-name: %s\n", fw_name);

	rproc = rproc_alloc(dev, np->name, &arm_rproc_ops, fw_name,
			    sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	priv = rproc->priv;
	priv->rproc = rproc;
	priv->core_dcfg = core_dcfg;

	res = platform_get_resource_byname(pdev,
					   IORESOURCE_MEM, "reset-control");
	priv->reset_cfg.ctrl_reg = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->reset_cfg.ctrl_reg)) {
		ret = PTR_ERR(priv->reset_cfg.ctrl_reg);
		dev_err(dev,
			"can't map the reset-control register (%pe)\n",
			ERR_PTR((unsigned long)priv->reset_cfg.ctrl_reg));
		goto err_free_rproc;
	} else {
		dev_dbg(dev, "reset-control: %p\n", priv->reset_cfg.ctrl_reg);
	}

	res = platform_get_resource_byname(pdev,
					   IORESOURCE_MEM, "reset-status");
	priv->reset_cfg.state_reg = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->reset_cfg.state_reg)) {
		ret = PTR_ERR(priv->reset_cfg.state_reg);
		dev_err(dev,
			"can't map the reset-status register (%pe)\n",
			ERR_PTR((unsigned long)priv->reset_cfg.state_reg));
		goto err_free_rproc;
	} else {
		dev_dbg(dev, "reset-status: %p\n",
			priv->reset_cfg.state_reg);
	}

	platform_set_drvdata(pdev, rproc);

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "can't add remote processor (%pe)\n",
			ERR_PTR(ret));
		goto err_free_rproc;
	} else {
		dev_dbg(dev, "remote processor added\n");
	}

	return 0;

err_free_rproc:
	rproc_free(rproc);

	return ret;
}

/**
 * arm_rproc_remove() - the platform device remove
 * @pdev: the platform device
 *
 * Delete and free the resources used.
 */
static void arm_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);
	rproc_free(rproc);
}

static const struct of_device_id arm_rproc_of_match[] = {
	{ .compatible = "arm,corstone1000-extsys", .data = &arm_rproc_cfg_corstone1000_extsys },
	{},
};
MODULE_DEVICE_TABLE(of, arm_rproc_of_match);

static struct platform_driver arm_rproc_driver = {
	.probe = arm_rproc_probe,
	.remove_new = arm_rproc_remove,
	.driver = {
		.name = "arm-rproc",
		.of_match_table = arm_rproc_of_match,
	},
};
module_platform_driver(arm_rproc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Arm Remote Processor Control Driver");
MODULE_AUTHOR("Abdellatif El Khlifi <abdellatif.elkhlifi@arm.com>");
