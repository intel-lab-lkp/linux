// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) IBM Corporation 2018
// FSI master driver for AST2600

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fsi.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/iopoll.h>
#include <linux/gpio/consumer.h>

#include "fsi-master.h"

struct fsi_master_aspeed_data {
	u32 opb_retry_counter;
};

struct fsi_master_aspeed {
	struct fsi_master	master;
	spinlock_t		lock;	/* protect HW access */
	struct irq_domain	*irq_domain;
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	struct gpio_desc	*cfam_reset_gpio;
	u32			irq_mask;
};

#define to_fsi_master_aspeed(m) \
	container_of(m, struct fsi_master_aspeed, master)

/* Control register (size 0x400) */
static const u32 ctrl_base = 0x80000000;

static const u32 fsi_base = 0xa0000000;

#define OPB_FSI_VER	0x00
#define OPB_TRIGGER	0x04
#define OPB_CTRL_BASE	0x08
#define OPB_FSI_BASE	0x0c
#define OPB_CLK_SYNC	0x3c
#define OPB_IRQ_CLEAR	0x40
#define OPB_IRQ_MASK	0x44
/*
 * This register does NOT behave in the expected manner. It is expected that writing 1b would clear
 * the corresponding interrupt condition. However it also invisibly masks the interrupt! Writing 0b
 * unmasks again.
 */
#define OPB_IRQ_STATUS	0x48

#define OPB0_SELECT	0x10
#define OPB0_RW		0x14
#define OPB0_XFER_SIZE	0x18
#define OPB0_FSI_ADDR	0x1c
#define OPB0_FSI_DATA_W	0x20
#define OPB0_STATUS	0x80
#define OPB0_FSI_DATA_R	0x84

#define OPB0_WRITE_ORDER1	0x4c
#define OPB0_WRITE_ORDER2	0x50
#define OPB1_WRITE_ORDER1	0x54
#define OPB1_WRITE_ORDER2	0x58
#define OPB0_READ_ORDER1	0x5c
#define OPB1_READ_ORDER2	0x60

#define OPB_RETRY_COUNTER	0x64
#define OPB_RETRY_COUNTER_AST2600	0x00010010
#define OPB_RETRY_COUNTER_AST2700	0x000d0010

/* OPBn_STATUS */
#define STATUS_HALFWORD_ACK	BIT(0)
#define STATUS_FULLWORD_ACK	BIT(1)
#define STATUS_ERR_ACK		BIT(2)
#define STATUS_RETRY		BIT(3)
#define STATUS_TIMEOUT		BIT(4)

/* OPB_IRQ_MASK */
#define FSI_MASTER_ERROR_IRQ	BIT(28)
#define FSI_PORT_ERROR_IRQ	BIT(27)
#define FSI_HOTPLUG_IRQ		BIT(26)
#define FSI_REMOTE_SLV_IRQ(l)	(BIT(FSI_REMOTE_SLV_IRQ_BIT) << (l))
#define FSI_REMOTE_SLV_IRQ_BIT	18
#define OPB1_XFER_ACK_EN BIT(17)
#define OPB0_XFER_ACK_EN BIT(16)

/* OPB_RW */
#define CMD_READ	BIT(0)
#define CMD_WRITE	0

/* OPBx_XFER_SIZE */
#define XFER_FULLWORD	(BIT(1) | BIT(0))
#define XFER_HALFWORD	(BIT(0))
#define XFER_BYTE	(0)

#define CREATE_TRACE_POINTS
#include <trace/events/fsi_master_aspeed.h>

#define OPB_POLL_TIMEOUT		500

static int __opb_write(struct fsi_master_aspeed *aspeed, u32 addr,
		       u32 val, u32 transfer_size)
{
	void __iomem *base = aspeed->base;
	u32 reg, status;
	int ret;

	/*
	 * The ordering of these writes up until the trigger
	 * write does not matter, so use writel_relaxed.
	 */
	writel_relaxed(CMD_WRITE, base + OPB0_RW);
	writel_relaxed(transfer_size, base + OPB0_XFER_SIZE);
	writel_relaxed(addr, base + OPB0_FSI_ADDR);
	writel_relaxed(val, base + OPB0_FSI_DATA_W);
	writel_relaxed(0, base + OPB_IRQ_STATUS);
	writel(0x1, base + OPB_TRIGGER);

	ret = readl_poll_timeout(base + OPB_IRQ_STATUS, reg,
				(reg & OPB0_XFER_ACK_EN) != 0,
				0, OPB_POLL_TIMEOUT);

	writel(OPB0_XFER_ACK_EN, base + OPB_IRQ_STATUS);
	status = readl(base + OPB0_STATUS);

	/* Return error when poll timed out */
	if (ret) {
		trace_fsi_master_aspeed_timeout(reg, status, false);
		return ret;
	}

	/* Command failed, master will reset */
	if (status & STATUS_ERR_ACK)
		return -EIO;

	trace_fsi_master_aspeed_opb_xfer(addr, transfer_size + 1, val, false);
	return 0;
}

static int opb_writeb(struct fsi_master_aspeed *aspeed, u32 addr, u8 val)
{
	return __opb_write(aspeed, addr, val, XFER_BYTE);
}

static int opb_writew(struct fsi_master_aspeed *aspeed, u32 addr, __be16 val)
{
	return __opb_write(aspeed, addr, (__force u16)val, XFER_HALFWORD);
}

static int opb_writel(struct fsi_master_aspeed *aspeed, u32 addr, __be32 val)
{
	return __opb_write(aspeed, addr, (__force u32)val, XFER_FULLWORD);
}

static int __opb_read(struct fsi_master_aspeed *aspeed, uint32_t addr,
		      u32 transfer_size, void *out)
{
	void __iomem *base = aspeed->base;
	u32 result, reg;
	int status, ret;

	/*
	 * The ordering of these writes up until the trigger
	 * write does not matter, so use writel_relaxed.
	 */
	writel_relaxed(CMD_READ, base + OPB0_RW);
	writel_relaxed(transfer_size, base + OPB0_XFER_SIZE);
	writel_relaxed(addr, base + OPB0_FSI_ADDR);
	writel_relaxed(0, aspeed->base + OPB_IRQ_STATUS);
	writel(0x1, base + OPB_TRIGGER);

	ret = readl_poll_timeout(base + OPB_IRQ_STATUS, reg,
			   (reg & OPB0_XFER_ACK_EN) != 0,
			   0, OPB_POLL_TIMEOUT);

	writel(OPB0_XFER_ACK_EN, base + OPB_IRQ_STATUS);
	status = readl(base + OPB0_STATUS);

	/* Return error when poll timed out */
	if (ret) {
		trace_fsi_master_aspeed_timeout(reg, status, true);
		return ret;
	}

	/* Command failed, master will reset */
	if (status & STATUS_ERR_ACK)
		return -EIO;

	result = readl(base + OPB0_FSI_DATA_R);

	switch (transfer_size) {
	case XFER_BYTE:
		*(u8 *)out = result;
		break;
	case XFER_HALFWORD:
		*(u16 *)out = result;
		break;
	case XFER_FULLWORD:
		*(u32 *)out = result;
		break;
	default:
		return -EINVAL;
	}

	trace_fsi_master_aspeed_opb_xfer(addr, transfer_size + 1, result, true);
	return 0;
}

static int opb_readl(struct fsi_master_aspeed *aspeed, uint32_t addr, __be32 *out)
{
	return __opb_read(aspeed, addr, XFER_FULLWORD, out);
}

static int opb_readw(struct fsi_master_aspeed *aspeed, uint32_t addr, __be16 *out)
{
	return __opb_read(aspeed, addr, XFER_HALFWORD, (void *)out);
}

static int opb_readb(struct fsi_master_aspeed *aspeed, uint32_t addr, u8 *out)
{
	return __opb_read(aspeed, addr, XFER_BYTE, (void *)out);
}

static int aspeed_master_read(struct fsi_master *master, int link,
			uint8_t id, uint32_t addr, void *val, size_t size)
{
	struct fsi_master_aspeed *aspeed = to_fsi_master_aspeed(master);
	unsigned long flags;
	int ret;

	if (id > 0x3)
		return -EINVAL;

	addr |= id << 21;
	addr += link * FSI_HUB_LINK_SIZE;

	spin_lock_irqsave(&aspeed->lock, flags);

	switch (size) {
	case 1:
		ret = opb_readb(aspeed, fsi_base + addr, val);
		break;
	case 2:
		ret = opb_readw(aspeed, fsi_base + addr, val);
		break;
	case 4:
		ret = opb_readl(aspeed, fsi_base + addr, val);
		break;
	default:
		ret = -EINVAL;
		goto done;
	}

	if (ret == -EIO)
		fsi_master_error(&aspeed->master, link);
done:
	spin_unlock_irqrestore(&aspeed->lock, flags);
	return ret;
}

static int aspeed_master_write(struct fsi_master *master, int link,
			uint8_t id, uint32_t addr, const void *val, size_t size)
{
	struct fsi_master_aspeed *aspeed = to_fsi_master_aspeed(master);
	unsigned long flags;
	int ret;

	if (id > 0x3)
		return -EINVAL;

	addr |= id << 21;
	addr += link * FSI_HUB_LINK_SIZE;

	spin_lock_irqsave(&aspeed->lock, flags);

	switch (size) {
	case 1:
		ret = opb_writeb(aspeed, fsi_base + addr, *(u8 *)val);
		break;
	case 2:
		ret = opb_writew(aspeed, fsi_base + addr, *(__be16 *)val);
		break;
	case 4:
		ret = opb_writel(aspeed, fsi_base + addr, *(__be32 *)val);
		break;
	default:
		ret = -EINVAL;
		goto done;
	}

	if (ret == -EIO)
		fsi_master_error(&aspeed->master, link);
done:
	spin_unlock_irqrestore(&aspeed->lock, flags);
	return ret;
}

static int aspeed_master_term(struct fsi_master *master, int link, uint8_t id)
{
	uint32_t addr;
	__be32 cmd;

	addr = 0x4;
	cmd = cpu_to_be32(0xecc00000);

	return aspeed_master_write(master, link, id, addr, &cmd, 4);
}

static int aspeed_master_break(struct fsi_master *master, int link)
{
	uint32_t addr;
	__be32 cmd;

	addr = 0x0;
	cmd = cpu_to_be32(0xc0de0000);

	return aspeed_master_write(master, link, 0, addr, &cmd, 4);
}

static int aspeed_master_link_enable(struct fsi_master *master, int link, bool enable)
{
	struct fsi_master_aspeed *aspeed = to_fsi_master_aspeed(master);
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&aspeed->lock, flags);
	if (enable) {
		rc = fsi_master_link_enable(master, link, enable);
		if (rc)
			goto done;

		aspeed->irq_mask |= FSI_REMOTE_SLV_IRQ(link);
		writel(aspeed->irq_mask, aspeed->base + OPB_IRQ_MASK);
	} else {
		aspeed->irq_mask &= ~FSI_REMOTE_SLV_IRQ(link);
		writel(aspeed->irq_mask, aspeed->base + OPB_IRQ_MASK);

		rc = fsi_master_link_enable(master, link, enable);
	}

done:
	spin_unlock_irqrestore(&aspeed->lock, flags);
	return rc;
}

static irqreturn_t aspeed_master_irq(int irq, void *data)
{
	struct fsi_master_aspeed *aspeed = data;
	unsigned long size = FSI_REMOTE_SLV_IRQ_BIT + aspeed->master.n_links;
	unsigned long bit = FSI_REMOTE_SLV_IRQ_BIT;
	unsigned long status;

	status = readl(aspeed->base + OPB_IRQ_STATUS);
	writel(0, aspeed->base + OPB_IRQ_MASK);

	for_each_set_bit_from(bit, &status, size)
		fsi_master_irq(&aspeed->master, aspeed->irq_domain, bit - FSI_REMOTE_SLV_IRQ_BIT);

	writel(status, aspeed->base + OPB_IRQ_STATUS);
	writel(0, aspeed->base + OPB_IRQ_STATUS);
	writel(aspeed->irq_mask, aspeed->base + OPB_IRQ_MASK);

	trace_fsi_master_aspeed_irq(status);
	return IRQ_HANDLED;
}

static int aspeed_master_irqd_map(struct irq_domain *domain, unsigned int irq,
				  irq_hw_number_t hwirq)
{
	struct fsi_master_aspeed *aspeed = domain->host_data;

	irq_set_chip_and_handler(irq, &aspeed->master.irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, &aspeed->master);

	return 0;
}

static const struct irq_domain_ops aspeed_master_irq_domain_ops = {
	.map = aspeed_master_irqd_map,
};

static void aspeed_master_release(struct device *dev)
{
	struct fsi_master_aspeed *aspeed =
		to_fsi_master_aspeed(to_fsi_master(dev));

	if (aspeed->irq_domain)
		irq_domain_remove(aspeed->irq_domain);

	regmap_exit(aspeed->master.map);
	kfree(aspeed);
}

static int regmap_aspeed_opb_read(void *context, unsigned int reg, unsigned int *val)
{
	__be32 v;
	int ret;

	ret = opb_readl(context, ctrl_base + reg, &v);
	if (ret)
		return ret;

	*val = be32_to_cpu(v);
	return 0;
}

static int regmap_aspeed_opb_write(void *context, unsigned int reg, unsigned int val)
{
	return opb_writel(context, ctrl_base + reg, cpu_to_be32(val));
}

static const struct regmap_bus regmap_aspeed_opb = {
	.reg_write = regmap_aspeed_opb_write,
	.reg_read = regmap_aspeed_opb_read,
};

static ssize_t cfam_reset_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fsi_master_aspeed *aspeed = dev_get_drvdata(dev);
	unsigned long flags;

	trace_fsi_master_aspeed_cfam_reset(true);
	spin_lock_irqsave(&aspeed->lock, flags);

	opb_writel(aspeed, ctrl_base + FSI_MMODE,
		   cpu_to_be32(aspeed->master.mmode & ~(FSI_MMODE_EIP | FSI_MMODE_RELA)));

	gpiod_set_value(aspeed->cfam_reset_gpio, 1);
	udelay(900);
	gpiod_set_value(aspeed->cfam_reset_gpio, 0);
	udelay(900);

	opb_writel(aspeed, ctrl_base + FSI_MRESP0, cpu_to_be32(FSI_MRESP_RST_ALL_MASTER));
	opb_writel(aspeed, ctrl_base + FSI_MMODE, cpu_to_be32(aspeed->master.mmode));

	spin_unlock_irqrestore(&aspeed->lock, flags);
	trace_fsi_master_aspeed_cfam_reset(false);

	return count;
}

static DEVICE_ATTR(cfam_reset, 0200, NULL, cfam_reset_store);

static int setup_cfam_reset(struct fsi_master_aspeed *aspeed)
{
	struct device *dev = aspeed->dev;
	struct gpio_desc *gpio;
	int rc;

	gpio = devm_gpiod_get_optional(dev, "cfam-reset", GPIOD_OUT_LOW);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);
	if (!gpio)
		return 0;

	aspeed->cfam_reset_gpio = gpio;

	rc = device_create_file(dev, &dev_attr_cfam_reset);
	if (rc) {
		aspeed->cfam_reset_gpio = NULL;
		devm_gpiod_put(dev, gpio);
		return rc;
	}

	return 0;
}

static int tacoma_cabled_fsi_fixup(struct device *dev)
{
	struct gpio_desc *routing_gpio, *mux_gpio;
	int gpio;

	/*
	 * The routing GPIO is a jumper indicating we should mux for the
	 * externally connected FSI cable.
	 */
	routing_gpio = devm_gpiod_get_optional(dev, "fsi-routing",
			GPIOD_IN | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(routing_gpio))
		return PTR_ERR(routing_gpio);
	if (!routing_gpio)
		return 0;

	mux_gpio = devm_gpiod_get_optional(dev, "fsi-mux", GPIOD_ASIS);
	if (IS_ERR(mux_gpio))
		return PTR_ERR(mux_gpio);
	if (!mux_gpio)
		return 0;

	gpio = gpiod_get_value(routing_gpio);
	if (gpio < 0)
		return gpio;

	/* If the routing GPIO is high we should set the mux to low. */
	if (gpio) {
		gpiod_direction_output(mux_gpio, 0);
		dev_info(dev, "FSI configured for external cable\n");
	} else {
		gpiod_direction_output(mux_gpio, 1);
	}

	devm_gpiod_put(dev, routing_gpio);

	return 0;
}

static int fsi_master_aspeed_probe(struct platform_device *pdev)
{
	const struct fsi_master_aspeed_data *md = of_device_get_match_data(&pdev->dev);
	u32 opb_retry_counter = md ? md->opb_retry_counter : OPB_RETRY_COUNTER_AST2600;
	struct regmap_config aspeed_master_regmap_config;
	struct fsi_master_aspeed *aspeed;
	unsigned int reg;
	int rc, links;
	int irq;

	rc = tacoma_cabled_fsi_fixup(&pdev->dev);
	if (rc) {
		dev_err(&pdev->dev, "Tacoma FSI cable fixup failed\n");
		return rc;
	}

	aspeed = kzalloc(sizeof(*aspeed), GFP_KERNEL);
	if (!aspeed)
		return -ENOMEM;

	aspeed->dev = &pdev->dev;
	spin_lock_init(&aspeed->lock);

	aspeed->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(aspeed->base)) {
		rc = PTR_ERR(aspeed->base);
		goto err_free_aspeed;
	}

	aspeed->clk = devm_clk_get(aspeed->dev, NULL);
	if (IS_ERR(aspeed->clk)) {
		dev_err(aspeed->dev, "couldn't get clock\n");
		rc = PTR_ERR(aspeed->clk);
		goto err_free_aspeed;
	}
	rc = clk_prepare_enable(aspeed->clk);
	if (rc) {
		dev_err(aspeed->dev, "couldn't enable clock\n");
		goto err_free_aspeed;
	}

	rc = setup_cfam_reset(aspeed);
	if (rc) {
		dev_err(&pdev->dev, "CFAM reset GPIO setup failed\n");
	}

	writel(0x1, aspeed->base + OPB_CLK_SYNC);

	writel(opb_retry_counter, aspeed->base + OPB_RETRY_COUNTER);

	writel(ctrl_base, aspeed->base + OPB_CTRL_BASE);
	writel(fsi_base, aspeed->base + OPB_FSI_BASE);

	/* Set read data order */
	writel(0x00030b1b, aspeed->base + OPB0_READ_ORDER1);

	/* Set write data order */
	writel(0x0011101b, aspeed->base + OPB0_WRITE_ORDER1);
	writel(0x0c330f3f, aspeed->base + OPB0_WRITE_ORDER2);

	/*
	 * Select OPB0 for all operations.
	 * Will need to be reworked when enabling DMA or anything that uses
	 * OPB1.
	 */
	writel(0x1, aspeed->base + OPB0_SELECT);

	fsi_master_regmap_config(&aspeed_master_regmap_config);
	aspeed->master.map = regmap_init(&pdev->dev, &regmap_aspeed_opb, aspeed,
					 &aspeed_master_regmap_config);
	if (IS_ERR(aspeed->master.map)) {
		rc = PTR_ERR(aspeed->master.map);
		goto err_release;
	}

	rc = regmap_read(aspeed->master.map, FSI_MVER, &reg);
	if (rc) {
		dev_err(&pdev->dev, "failed to read hub version\n");
		goto err_regmap;
	}

	links = (reg >> 8) & 0xff;
	dev_info(&pdev->dev, "hub version %08x (%d links)\n", reg, links);

	aspeed->master.dev.parent = &pdev->dev;
	aspeed->master.dev.release = aspeed_master_release;
	aspeed->master.dev.of_node = of_node_get(dev_of_node(&pdev->dev));

	aspeed->master.n_links = links;
	aspeed->master.flags = FSI_MASTER_FLAG_INTERRUPT | FSI_MASTER_FLAG_RELA;
	aspeed->master.read = aspeed_master_read;
	aspeed->master.write = aspeed_master_write;
	aspeed->master.send_break = aspeed_master_break;
	aspeed->master.term = aspeed_master_term;
	aspeed->master.link_enable = aspeed_master_link_enable;

	dev_set_drvdata(&pdev->dev, aspeed);

	rc = fsi_master_init(&aspeed->master, clk_get_rate(aspeed->clk));
	if (rc)
		goto err_regmap;

	irq = platform_get_irq(pdev, 0);
	if (irq > 0) {
		unsigned int size = links * FSI_IRQ_COUNT;

		aspeed->irq_domain = irq_domain_add_linear(aspeed->dev->of_node, size,
							   &aspeed_master_irq_domain_ops, aspeed);
		if (aspeed->irq_domain) {
			rc = devm_request_irq(aspeed->dev, irq, aspeed_master_irq, 0,
					      dev_name(aspeed->dev), aspeed);
			if (rc) {
				dev_warn(aspeed->dev, "failed to request irq:%d\n", irq);
				irq_domain_remove(aspeed->irq_domain);
				aspeed->irq_domain = NULL;
			} else {
				dev_info(aspeed->dev, "enabling interrupts irq:%d\n", irq);
			}
		} else {
			dev_warn(aspeed->dev, "failed to create irq domain\n");
		}
	}

	rc = fsi_master_register(&aspeed->master);
	if (rc)
		goto err_irq;

	/* At this point, fsi_master_register performs the device_initialize(),
	 * and holds the sole reference on master.dev. This means the device
	 * will be freed (via ->release) during any subsequent call to
	 * fsi_master_unregister.  We add our own reference to it here, so we
	 * can perform cleanup (in _remove()) without it being freed before
	 * we're ready.
	 */
	get_device(&aspeed->master.dev);
	return 0;

err_irq:
	if (aspeed->irq_domain)
		irq_domain_remove(aspeed->irq_domain);
err_regmap:
	regmap_exit(aspeed->master.map);
err_release:
	if (aspeed->cfam_reset_gpio)
		device_remove_file(aspeed->dev, &dev_attr_cfam_reset);

	clk_disable_unprepare(aspeed->clk);
err_free_aspeed:
	kfree(aspeed);
	return rc;
}

static int fsi_master_aspeed_remove(struct platform_device *pdev)
{
	struct fsi_master_aspeed *aspeed = platform_get_drvdata(pdev);

	if (aspeed->cfam_reset_gpio)
		device_remove_file(aspeed->dev, &dev_attr_cfam_reset);

	fsi_master_unregister(&aspeed->master);
	clk_disable_unprepare(aspeed->clk);

	return 0;
}

static const struct fsi_master_aspeed_data fsi_master_ast2600_data = {
	.opb_retry_counter = OPB_RETRY_COUNTER_AST2600,
};

static const struct fsi_master_aspeed_data fsi_master_ast2700_data = {
	.opb_retry_counter = OPB_RETRY_COUNTER_AST2700,
};

static const struct of_device_id fsi_master_aspeed_match[] = {
	{
		.compatible = "aspeed,ast2600-fsi-master",
		.data = &fsi_master_ast2600_data,
	},
	{
		.compatible = "aspeed,ast2700-fsi-master",
		.data = &fsi_master_ast2700_data,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, fsi_master_aspeed_match);

static struct platform_driver fsi_master_aspeed_driver = {
	.driver = {
		.name		= "fsi-master-aspeed",
		.of_match_table	= fsi_master_aspeed_match,
	},
	.probe	= fsi_master_aspeed_probe,
	.remove = fsi_master_aspeed_remove,
};

module_platform_driver(fsi_master_aspeed_driver);
MODULE_LICENSE("GPL");
