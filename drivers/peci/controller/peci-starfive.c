// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 StarFive Technology Co., Ltd.
 */

#include <linux/unaligned.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/peci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

/* Control register */
#define STARFIVE_PECI_CTRL			0x00
/* automatically clears after transfer started */
#define   STARFIVE_PECI_CTRL_START		BIT(0)
#define   STARFIVE_PECI_CTRL_FCS_MODE		BIT(2)
#define   STARFIVE_PECI_CTRL_AW_FCS_EN		BIT(3)
#define   STARFIVE_PECI_CTRL_RDY		BIT(4)

/* Read/Write length register */
#define STARFIVE_PECI_HDR			0x04
#define   STARFIVE_PECI_HDR_RD_LEN_MASK		GENMASK(23, 16)
#define   STARFIVE_PECI_HDR_WR_LEN_MASK		GENMASK(15, 8)
#define   STARFIVE_PECI_HDR_TARGET_ADDR_MASK	GENMASK(7, 0)

/* Feature control register */
#define STARFIVE_PECI_F_CTRL			0x0c
#define   STARFIVE_PECI_F_CTRL_EN		BIT(0)

/* Interrupt enable register */
#define STARFIVE_PECI_INT_EN			0x10
#define   STARFIVE_PECI_INT_XFER_DONE		BIT(0)
#define   STARFIVE_PECI_INT_CFG_ERR		BIT(1)
#define   STARFIVE_PECI_INT_TBIT_ERR		BIT(2)
#define   STARFIVE_PECI_INT_BAD_WR_FCS		BIT(3)
#define   STARFIVE_PECI_INT_ABORT_WR_FCS	BIT(4)
#define   STARFIVE_PECI_INT_BAD_RD_FCS		BIT(5)
#define   STARFIVE_PECI_INT_BUS_CONTENTION	BIT(6)
#define   STARFIVE_PECI_INT_TBIT_OVER_LIMIT	BIT(7)
#define   STARFIVE_PECI_INT_ALL			GENMASK(7, 0)

/* Interrupt status register */
#define STARFIVE_PECI_INT_STS			0x14
#define   STARFIVE_PECI_STS_XFER_DONE		BIT(0)
#define   STARFIVE_PECI_STS_CFG_ERR		BIT(1)
#define   STARFIVE_PECI_STS_TBIT_ERR		BIT(2)
#define   STARFIVE_PECI_STS_BAD_WR_FCS		BIT(3)
#define   STARFIVE_PECI_STS_ABORT_WR_FCS	BIT(4)
#define   STARFIVE_PECI_STS_BAD_RD_FCS		BIT(5)
#define   STARFIVE_PECI_STS_BUS_CONTENTION	BIT(6)
#define   STARFIVE_PECI_STS_TBIT_OVER_LIMIT	BIT(7)
#define   STARFIVE_PECI_STS_MASK		GENMASK(7, 0)

/* Received FCS data register */
#define STARFIVE_PECI_FCS_RCVD			0x1C
#define   STARFIVE_PECI_RCVD_WR_FCS_MASK	GENMASK(7, 0)
#define   STARFIVE_PECI_RCVD_RD_FCS_MASK	GENMASK(15, 8)

/* Rx/Tx Data Buffer Registers */
#define STARFIVE_PECI_WR_DATA(n)		(0x70 + ((n) * 4))
#define STARFIVE_PECI_RD_DATA(n)		(0xB0 + ((n) * 4))

/* Hardware TX/RX data FIFOs are 64 bytes, but PECI core caps requests lower */
#define STARFIVE_PECI_DATA_BUF_SIZE_MAX		min(64, PECI_REQUEST_MAX_BUF_SIZE)
#define STARFIVE_PECI_MAX_REG			0x100

/* Timeout */
#define STARFIVE_PECI_IDLE_CHECK_TIMEOUT_US	(50 * USEC_PER_MSEC)
#define STARFIVE_PECI_IDLE_CHECK_INTERVAL_US	(10 * USEC_PER_MSEC)
#define STARFIVE_PECI_CMD_TIMEOUT_MS_DEFAULT	1000
#define STARFIVE_PECI_CMD_TIMEOUT_MS_MAX	1000

struct starfive_peci {
	u32			cmd_timeout_ms;
	struct completion	xfer_complete;
	struct regmap		*regmap;
	u32			status;
	spinlock_t		lock;	/* sync completion status */
	struct peci_controller	*controller;
	struct device		*dev;
	struct clk		*clk;
	struct reset_control	*rst;
	int			irq;
};

static int starfive_peci_xfer(struct peci_controller *controller,
			      u8 addr, struct peci_request *req)
{
	struct starfive_peci *priv = dev_get_drvdata(controller->dev.parent);
	unsigned long timeout = msecs_to_jiffies(priv->cmd_timeout_ms);
	u32 msg_rd;
	u32 cmd_sts;
	u32 peci_hdr;
	int i, ret, j;

	if (req->tx.len > STARFIVE_PECI_DATA_BUF_SIZE_MAX ||
	    req->rx.len > STARFIVE_PECI_DATA_BUF_SIZE_MAX)
		return -EINVAL;

	/* Check command sts and bus idle state */
	ret = regmap_read_poll_timeout(priv->regmap,
				       STARFIVE_PECI_CTRL, cmd_sts,
				       cmd_sts & STARFIVE_PECI_CTRL_RDY,
				       STARFIVE_PECI_IDLE_CHECK_INTERVAL_US,
				       STARFIVE_PECI_IDLE_CHECK_TIMEOUT_US);
	if (ret)
		return ret;

	spin_lock_irq(&priv->lock);
	reinit_completion(&priv->xfer_complete);

	peci_hdr = FIELD_PREP(STARFIVE_PECI_HDR_TARGET_ADDR_MASK, addr) |
		   FIELD_PREP(STARFIVE_PECI_HDR_WR_LEN_MASK, req->tx.len) |
		   FIELD_PREP(STARFIVE_PECI_HDR_RD_LEN_MASK, req->rx.len);
	regmap_write(priv->regmap, STARFIVE_PECI_HDR, peci_hdr);

	/* Currently, we do not support write commands, force this bit to 0. */
	regmap_write_bits(priv->regmap, STARFIVE_PECI_CTRL,
			  STARFIVE_PECI_CTRL_AW_FCS_EN, 0);

	for (i = 0; i < ALIGN(req->tx.len, 4) / 4; i++)
		regmap_write(priv->regmap, STARFIVE_PECI_WR_DATA(i),
			     get_unaligned_le32(&req->tx.buf[4 * i]));

	dev_dbg(priv->dev, "addr : %#02x, tx.len : %#02x, rx.len : %#02x\n",
		addr, req->tx.len, req->rx.len);
	print_hex_dump_bytes("TX : ", DUMP_PREFIX_NONE, req->tx.buf,
			     req->tx.len);

	priv->status = 0;

	regmap_write_bits(priv->regmap, STARFIVE_PECI_CTRL,
			  STARFIVE_PECI_CTRL_START, STARFIVE_PECI_CTRL_START);

	spin_unlock_irq(&priv->lock);

	ret = wait_for_completion_interruptible_timeout(&priv->xfer_complete,
							timeout);
	if (ret < 0)
		return ret;

	if (ret == 0) {
		dev_dbg(priv->dev, "Timeout waiting for a response\n");
		return -ETIMEDOUT;
	}

	spin_lock_irq(&priv->lock);

	if (priv->status != STARFIVE_PECI_STS_XFER_DONE) {
		spin_unlock_irq(&priv->lock);
		dev_dbg(priv->dev, "No valid response, status: %#02x\n",
			priv->status);
		return -EIO;
	}

	regmap_read(priv->regmap, STARFIVE_PECI_FCS_RCVD, &msg_rd);
	dev_dbg(priv->dev, "write & read command FCS : %#02lx & %#02lx\n",
		FIELD_GET(STARFIVE_PECI_RCVD_WR_FCS_MASK, msg_rd),
		FIELD_GET(STARFIVE_PECI_RCVD_RD_FCS_MASK, msg_rd));

	for (i = 0; i < ALIGN(req->rx.len, 4) / 4; i++) {
		regmap_read(priv->regmap, STARFIVE_PECI_RD_DATA(i), &msg_rd);

		if (req->rx.len - 4 * (i + 1) >= 0) {
			put_unaligned_be32(msg_rd, &req->rx.buf[req->rx.len - 4 * (i + 1)]);
		} else {
			for (j = req->rx.len % 4 - 1; j >= 0; j--) {
				req->rx.buf[j] = (u8)msg_rd;
				msg_rd >>= 8;
			}
		}
	}

	spin_unlock_irq(&priv->lock);

	print_hex_dump_bytes("RX : ",
			     DUMP_PREFIX_NONE, req->rx.buf, req->rx.len);

	return 0;
}

static irqreturn_t starfive_peci_irq_handler(int irq, void *arg)
{
	struct starfive_peci *priv = arg;
	u32 status;

	spin_lock(&priv->lock);
	regmap_read(priv->regmap, STARFIVE_PECI_INT_STS, &status);
	priv->status |= (status & STARFIVE_PECI_STS_MASK);
	regmap_write(priv->regmap, STARFIVE_PECI_INT_STS,
		     status & STARFIVE_PECI_STS_MASK);

	/*
	 * All commands are terminated with the XFER_DONE bit set, even when
	 * an error condition is reported alongside it.
	 */
	if (status & STARFIVE_PECI_STS_XFER_DONE)
		complete(&priv->xfer_complete);

	spin_unlock(&priv->lock);

	return IRQ_HANDLED;
}

static void starfive_peci_clk_reset_release(void *data)
{
	struct starfive_peci *priv = data;

	clk_disable_unprepare(priv->clk);
	reset_control_assert(priv->rst);
}

static int starfive_peci_init_clk_rst(struct starfive_peci *priv)
{
	int ret;

	priv->clk = devm_clk_get(priv->dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(priv->dev, PTR_ERR(priv->clk),
				     "Failed to get peci clock\n");

	priv->rst = devm_reset_control_get_exclusive(priv->dev, NULL);
	if (IS_ERR(priv->rst))
		return dev_err_probe(priv->dev, PTR_ERR(priv->rst),
				     "Failed to get reset control\n");

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "Failed to enable peci clock\n");

	ret = reset_control_deassert(priv->rst);
	if (ret) {
		clk_disable_unprepare(priv->clk);
		return dev_err_probe(priv->dev, ret,
				     "Failed to deassert reset control\n");
	}

	return devm_add_action_or_reset(priv->dev, starfive_peci_clk_reset_release,
					priv);
}

static int starfive_peci_init_ctrl(struct starfive_peci *priv)
{
	u32 cmd_sts;
	int ret;

	ret = device_property_read_u32(priv->dev, "cmd-timeout-ms",
				       &priv->cmd_timeout_ms);
	if (ret) {
		priv->cmd_timeout_ms = STARFIVE_PECI_CMD_TIMEOUT_MS_DEFAULT;
	} else if (priv->cmd_timeout_ms > STARFIVE_PECI_CMD_TIMEOUT_MS_MAX ||
		   priv->cmd_timeout_ms == 0) {
		dev_warn(priv->dev,
			 "Invalid cmd-timeout-ms: %u, falling back to: %u\n",
			 priv->cmd_timeout_ms,
			 STARFIVE_PECI_CMD_TIMEOUT_MS_DEFAULT);

		priv->cmd_timeout_ms = STARFIVE_PECI_CMD_TIMEOUT_MS_DEFAULT;
	}

	regmap_write_bits(priv->regmap, STARFIVE_PECI_F_CTRL,
			  STARFIVE_PECI_F_CTRL_EN, STARFIVE_PECI_F_CTRL_EN);
	regmap_write_bits(priv->regmap, STARFIVE_PECI_CTRL,
			  STARFIVE_PECI_CTRL_FCS_MODE, STARFIVE_PECI_CTRL_FCS_MODE);

	ret = regmap_read_poll_timeout(priv->regmap,
				       STARFIVE_PECI_CTRL, cmd_sts,
				       cmd_sts & STARFIVE_PECI_CTRL_RDY,
				       STARFIVE_PECI_IDLE_CHECK_INTERVAL_US,
				       STARFIVE_PECI_IDLE_CHECK_TIMEOUT_US);
	if (ret)
		return ret;

	regmap_write(priv->regmap, STARFIVE_PECI_INT_STS, STARFIVE_PECI_STS_MASK);

	/* PECI interrupt enable */
	regmap_write(priv->regmap, STARFIVE_PECI_INT_EN, STARFIVE_PECI_INT_ALL);

	return 0;
}

static const struct regmap_config starfive_peci_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.max_register = STARFIVE_PECI_MAX_REG,
	.fast_io = true,
};

static const struct peci_controller_ops starfive_ops = {
	.xfer = starfive_peci_xfer,
};

static int starfive_peci_probe(struct platform_device *pdev)
{
	struct peci_controller *controller;
	struct starfive_peci *priv;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	dev_set_drvdata(&pdev->dev, priv);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					     &starfive_peci_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(priv->dev, PTR_ERR(priv->regmap),
				     "Failed to initialize regmap\n");

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(priv->dev, priv->irq, "Failed to get IRQ\n");

	init_completion(&priv->xfer_complete);
	spin_lock_init(&priv->lock);

	ret = starfive_peci_init_clk_rst(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "Failed to initialize clock and reset\n");

	ret = devm_request_irq(&pdev->dev,
			       priv->irq, starfive_peci_irq_handler,
			       0, "peci-starfive", priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "Failed to request IRQ\n");

	ret = starfive_peci_init_ctrl(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "Failed to initialize control\n");

	controller = devm_peci_controller_add(priv->dev, &starfive_ops);
	if (IS_ERR(controller))
		return dev_err_probe(priv->dev, PTR_ERR(controller),
				     "Failed to add peci controller\n");

	priv->controller = controller;

	return 0;
}

static const struct of_device_id starfive_peci_of_table[] = {
	{ .compatible = "starfive,jhb100-peci", },
	{ }
};
MODULE_DEVICE_TABLE(of, starfive_peci_of_table);

static struct platform_driver starfive_peci_driver = {
	.probe	= starfive_peci_probe,
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table = starfive_peci_of_table,
	},
};
module_platform_driver(starfive_peci_driver);

MODULE_AUTHOR("Changhuang Liang <changhuang.liang@starfivetech.com>");
MODULE_AUTHOR("Mason Huo <mason.huo@starfivetech.com>");
MODULE_DESCRIPTION("StarFive PECI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PECI");
