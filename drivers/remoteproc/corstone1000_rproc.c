// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm Corstone-1000 Remoteproc driver
 *
 * The driver adds remoteproc support for the external cores used in Arm
 * Corstone-1000 IoT Reference Design Platform [1][2]
 * [1] Arm Corstone-1000 Technical Overview: https://developer.arm.com/documentation/102360/0000
 * [2] Arm Corstone SSE-710 Subsystem Technical Reference Manual: https://developer.arm.com/documentation/102342/0000
 *
 * Copyright (C) 2024 ARM Ltd.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>

#include "remoteproc_internal.h"

/**
 * struct corstone1000_rproc - Arm remote processor instance
 * @rproc: rproc handler
 * @regmap: MMIO register map
 * @ctrl_reg: control register offset
 * @state_reg: state register offset
 */
struct corstone1000_rproc {
	struct rproc *rproc;
	struct regmap *regmap;
	u16 ctrl_reg;
	u16 state_reg;
};

/* Definitions for Arm Corstone-1000 External System */

/* External Systems identifiers */
#define EXT_SYS0_ID			(0)         /* External System 0 ID */
#define EXT_SYS1_ID			(1)         /* External System 1 ID */

/* External System 0 registers offset */
#define EXT_SYS0_RST_CTRL		(0x310)     /* Reset Control register */
#define EXT_SYS0_RST_ST			(0x314)     /* Reset Status register */

/* External System 1 registers offset */
#define EXT_SYS1_RST_CTRL		(0x318)     /* Reset Control register */
#define EXT_SYS1_RST_ST			(0x31c)     /* Reset Status register */

/* External System Reset Control register bit definitions */
#define EXTSYS_RST_CTRL_CPUWAIT		BIT(0)      /* CPU Wait control */
#define EXTSYS_RST_CTRL_RST_REQ		BIT(1)      /*Reset request */

/* Status of reset request bits */
#define EXTSYS_RST_ACK_MASK		GENMASK(2, 1)
#define GET_EXTSYS_RST_ST_RST_ACK(x)	((u8)(FIELD_GET(EXTSYS_RST_ACK_MASK, \
					(x))))

/* Possible values for the Status of reset request */
#define EXTSYS_RST_ACK_NO_RESET_REQ	(0x0)
#define EXTSYS_RST_ACK_NOT_COMPLETE	(0x1)
#define EXTSYS_RST_ACK_COMPLETE		(0x2)
#define EXTSYS_RST_ACK_RESERVED		(0x3)

/* Polling settings used when reading the  Status of reset request */
#define EXTSYS_RST_ACK_POLL_TRIES	(3)
#define EXTSYS_RST_ACK_POLL_TIMEOUT	(1000)

static int corstone1000_rproc_extsys_poll_rst_ack(struct rproc *rproc,
						  u8 exp_ack, u8 *rst_ack);

/**
 * corstone1000_rproc_start() - custom start operation
 * @rproc: pointer to the remote processor object
 *
 * Start function for Corstone-1000 External System.
 * Allow the External System core start execute instructions.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int corstone1000_rproc_start(struct rproc *rproc)
{
	struct corstone1000_rproc *priv = rproc->priv;

	/* CPUWAIT signal of the External System is de-asserted */

	return regmap_write_bits(priv->regmap, priv->ctrl_reg,
				EXTSYS_RST_CTRL_CPUWAIT,
				(unsigned int)~EXTSYS_RST_CTRL_CPUWAIT);
}

/**
 * corstone1000_rproc_stop() - custom stop operation
 * @rproc: pointer to the remote processor object
 *
 * Reset all logic within the External System, the core will be in a halt state.
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int corstone1000_rproc_stop(struct rproc *rproc)
{
	struct corstone1000_rproc *priv = rproc->priv;
	u8 rst_ack, req_status;
	int ret;

	ret = regmap_write_bits(priv->regmap, priv->ctrl_reg,
				EXTSYS_RST_CTRL_RST_REQ,
				EXTSYS_RST_CTRL_RST_REQ);
	if (ret)
		return ret;

	ret = corstone1000_rproc_extsys_poll_rst_ack(rproc,
						     EXTSYS_RST_ACK_COMPLETE |
						     EXTSYS_RST_ACK_NOT_COMPLETE,
						     &rst_ack);
	if (ret)
		return ret;

	req_status = rst_ack;

	ret = regmap_write_bits(priv->regmap, priv->ctrl_reg,
				EXTSYS_RST_CTRL_RST_REQ,
				(unsigned int)~EXTSYS_RST_CTRL_RST_REQ);
	if (ret)
		return ret;

	ret = corstone1000_rproc_extsys_poll_rst_ack(rproc,
						     EXTSYS_RST_ACK_NO_RESET_REQ,
						     &rst_ack);
	if (ret)
		return ret;

	if (req_status == EXTSYS_RST_ACK_COMPLETE)
		return 0;

	return -EACCES;
}

/**
 * corstone1000_rproc_parse_fw() - Parse firmware operation
 * @rproc: pointer to the remote processor object
 * @fw: pointer to the firmware
 *
 * Does nothing currently. No resource information needed from the firmware
 * image.
 *
 * Return:
 *
 * 0 for success.
 */
static int corstone1000_rproc_parse_fw(struct rproc *rproc,
				       const struct firmware *fw)
{
	return 0;
}

/**
 * corstone1000_rproc_load() - Load firmware to memory operation
 * @rproc: pointer to the remote processor object
 * @fw: pointer to the firmware
 *
 * The remoteproc subsystem expects a firmware image to be provided and loaded
 * into memory. In case of Corstone-1000, the firmware is already loaded before
 * the Corstone-1000 is powered on and this is done through the FPGA board
 * bootloader in case of the FPGA target. Or by the Corstone-1000 FVP model
 * when using the FVP target.
 *
 * Return:
 *
 * 0 for success.
 */
static int corstone1000_rproc_load(struct rproc *rproc,
				   const struct firmware *fw)
{
	return 0;
}

static const struct rproc_ops corstone1000_rproc_ops = {
	.start		= corstone1000_rproc_start,
	.stop		= corstone1000_rproc_stop,
	.load		= corstone1000_rproc_load,
	.parse_fw	= corstone1000_rproc_parse_fw,
};

/**
 * corstone1000_rproc_extsys_poll_rst_ack() - poll RST_ACK bits
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
static int corstone1000_rproc_extsys_poll_rst_ack(struct rproc *rproc,
						  u8 exp_ack, u8 *rst_ack)
{
	struct device *dev;
	struct corstone1000_rproc *priv;
	int tries = EXTSYS_RST_ACK_POLL_TRIES;
	int ret;
	unsigned long timeout;
	u32 state_reg;

	if (!rproc || !rst_ack)
		return -ENODATA;

	dev = rproc->dev.parent;
	priv = rproc->priv;

	do {
		ret = regmap_read(priv->regmap, priv->state_reg, &state_reg);
		if (ret)
			return ret;

		*rst_ack = GET_EXTSYS_RST_ST_RST_ACK(state_reg);

		if (*rst_ack == EXTSYS_RST_ACK_RESERVED) {
			dev_err(dev, "unexpected RST_ACK value: 0x%x\n",
				*rst_ack);
			return -EINVAL;
		}

		/*
		 * when RST_REQ bit is cleared (No reset requested)
		 * RST_ACK is expected to be 00 (No reset requested)
		 */
		if (exp_ack == EXTSYS_RST_ACK_NO_RESET_REQ &&
		    *rst_ack == exp_ack)
			return 0;

		/*
		 * when a reset is requested (RST_REQ set) , RST_ACK is either
		 * 01 (Reset request unable to complete) or 10 (Reset request
		 * complete)
		 */
		if (*rst_ack & exp_ack)
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
 * corstone1000_rproc_probe() - the platform device probe
 * @pdev: the platform device
 *
 * Setup an rproc device and regmap using syscon
 *
 * Return:
 *
 * 0 on success. Otherwise, failure
 */
static int corstone1000_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *parent;
	struct corstone1000_rproc *priv;
	struct rproc *rproc;
	const char *fw_name;
	struct regmap *regmap;
	int ret;
	u8 extsys_id = EXT_SYS0_ID;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret)
		return dev_err_probe(dev, ret,
				     "can't parse firmware-name from DT\n");

	dev_dbg(dev, "firmware-name: %s\n", fw_name);

	rproc = devm_rproc_alloc(dev, np->name, &corstone1000_rproc_ops, fw_name,
				 sizeof(*priv));
	if (!rproc)
		return dev_err_probe(dev, -ENOMEM,
				     "can't allocate rproc device\n");

	priv = rproc->priv;
	priv->rproc = rproc;

	parent = of_get_parent(dev->of_node); /* parent is syscon node */
	regmap = syscon_node_to_regmap(parent);
	of_node_put(parent);
	if (IS_ERR_OR_NULL(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "can't read syscon node\n");
	priv->regmap = regmap;

	ret = of_property_read_u8(np, "#extsys-id", &extsys_id);
	if (ret)
		return dev_err_probe(dev, ret, "can't read '#extsys-id' property\n");

	if (extsys_id == EXT_SYS0_ID) {
		priv->ctrl_reg = EXT_SYS0_RST_CTRL;
		priv->state_reg = EXT_SYS0_RST_ST;
	} else {
		priv->ctrl_reg = EXT_SYS1_RST_CTRL;
		priv->state_reg = EXT_SYS1_RST_ST;
	}

	platform_set_drvdata(pdev, priv);

	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "can't add remote processor\n");

	return 0;
}

static const struct of_device_id corstone1000_rproc_of_match[] = {
	{ .compatible = "arm,sse710-extsys"},
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, corstone1000_rproc_of_match);

static struct platform_driver corstone1000_rproc_driver = {
	.probe = corstone1000_rproc_probe,
	.driver = {
		.name = "corstone1000-rproc",
		.of_match_table = corstone1000_rproc_of_match,
	},
};
module_platform_driver(corstone1000_rproc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Arm Corstone-1000 Remote Processor Control Driver");
MODULE_AUTHOR("Abdellatif El Khlifi <abdellatif.elkhlifi@arm.com>");
