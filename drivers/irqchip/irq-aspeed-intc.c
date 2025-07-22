// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Aspeed Interrupt Controller.
 *
 *  Copyright (C) 2023 ASPEED Technology Inc.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#define INTC0_ROUTING0_SEL0	0x200
#define INTC0_ROUTING0_SEL1	0x300
#define INTC0_ROUTING0_SEL2	0x400
#define INTC1_ROUTING0_SEL0	0x80
#define INTC1_ROUTING0_SEL1	0xa0
#define INTC1_ROUTING0_SEL2	0xc0

#define INTC_INT_ENABLE_REG	0x00
#define INTC_INT_STATUS_REG	0x04
#define INTC_IRQS_PER_WORD	32

struct aspeed_intc_ic {
	void __iomem		*base;
	raw_spinlock_t		gic_lock;
	raw_spinlock_t		intc_lock;
	struct irq_domain	*irq_domain;
};

static void aspeed_intc_ic_irq_handler(struct irq_desc *desc)
{
	struct aspeed_intc_ic *intc_ic = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);

	scoped_guard(raw_spinlock, &intc_ic->gic_lock) {
		unsigned long bit, status;

		status = readl(intc_ic->base + INTC_INT_STATUS_REG);
		for_each_set_bit(bit, &status, INTC_IRQS_PER_WORD) {
			generic_handle_domain_irq(intc_ic->irq_domain, bit);
			writel(BIT(bit), intc_ic->base + INTC_INT_STATUS_REG);
		}
	}

	chained_irq_exit(chip, desc);
}

static void aspeed_intc_irq_mask(struct irq_data *data)
{
	struct aspeed_intc_ic *intc_ic = irq_data_get_irq_chip_data(data);
	unsigned int mask = readl(intc_ic->base + INTC_INT_ENABLE_REG) & ~BIT(data->hwirq);

	guard(raw_spinlock)(&intc_ic->intc_lock);
	writel(mask, intc_ic->base + INTC_INT_ENABLE_REG);
}

static void aspeed_intc_irq_unmask(struct irq_data *data)
{
	struct aspeed_intc_ic *intc_ic = irq_data_get_irq_chip_data(data);
	unsigned int unmask = readl(intc_ic->base + INTC_INT_ENABLE_REG) | BIT(data->hwirq);

	guard(raw_spinlock)(&intc_ic->intc_lock);
	writel(unmask, intc_ic->base + INTC_INT_ENABLE_REG);
}

static struct irq_chip aspeed_intc_chip = {
	.name			= "ASPEED INTC",
	.irq_mask		= aspeed_intc_irq_mask,
	.irq_unmask		= aspeed_intc_irq_unmask,
};

static int aspeed_intc_ic_map_irq_domain(struct irq_domain *domain, unsigned int irq,
					 irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &aspeed_intc_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops aspeed_intc_ic_irq_domain_ops = {
	.map = aspeed_intc_ic_map_irq_domain,
};

static int __init aspeed_intc_ic_of_init(struct device_node *node,
					 struct device_node *parent)
{
	struct aspeed_intc_ic *intc_ic;
	int irq, i, ret = 0;

	intc_ic = kzalloc(sizeof(*intc_ic), GFP_KERNEL);
	if (!intc_ic)
		return -ENOMEM;

	intc_ic->base = of_iomap(node, 0);
	if (!intc_ic->base) {
		pr_err("Failed to iomap intc_ic base\n");
		ret = -ENOMEM;
		goto err_free_ic;
	}
	writel(0xffffffff, intc_ic->base + INTC_INT_STATUS_REG);
	writel(0x0, intc_ic->base + INTC_INT_ENABLE_REG);

	intc_ic->irq_domain = irq_domain_create_linear(of_fwnode_handle(node), INTC_IRQS_PER_WORD,
						    &aspeed_intc_ic_irq_domain_ops, intc_ic);
	if (!intc_ic->irq_domain) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	raw_spin_lock_init(&intc_ic->gic_lock);
	raw_spin_lock_init(&intc_ic->intc_lock);

	/* Check all the irq numbers valid. If not, unmaps all the base and frees the data. */
	for (i = 0; i < of_irq_count(node); i++) {
		irq = irq_of_parse_and_map(node, i);
		if (!irq) {
			pr_err("Failed to get irq number\n");
			ret = -EINVAL;
			goto err_iounmap;
		}
	}

	for (i = 0; i < of_irq_count(node); i++) {
		irq = irq_of_parse_and_map(node, i);
		irq_set_chained_handler_and_data(irq, aspeed_intc_ic_irq_handler, intc_ic);
	}

	return 0;

err_iounmap:
	iounmap(intc_ic->base);
err_free_ic:
	kfree(intc_ic);
	return ret;
}

IRQCHIP_DECLARE(ast2700_intc_ic, "aspeed,ast2700-intc-ic", aspeed_intc_ic_of_init);

struct aspeed_intc {
	void __iomem *base;
	struct device *dev;
	struct dentry *dbg_root;
	int (*show_routing)(struct seq_file *s, void *unused);
	int (*show_prot)(struct seq_file *s, void *unused);
};

/*
 * 000: Route interrupt INTn to PSP GICINT0-31
 * 001: Route interrupt INTn to SSPINT0-31
 * 010: Route interrupt INTn to TSPINT0-31
 */
static int aspeed_intc0_show_routing(struct seq_file *s, void *unused)
{
	struct aspeed_intc *intc = s->private;
	int group, bit;

	seq_puts(s, "int | PSP | SSP | TSP\n");
	seq_puts(s, "----+-----+-----+----\n");

	for (group = 0; group < 4; group++) {
		u32 reg0 = readl(intc->base + INTC0_ROUTING0_SEL0 + group * 4);
		u32 reg1 = readl(intc->base + INTC0_ROUTING0_SEL1 + group * 4);
		u32 reg2 = readl(intc->base + INTC0_ROUTING0_SEL2 + group * 4);

		for (bit = 0; bit < 32; bit++) {
			int idx = group * 32 + bit;
			u8 routing = (((reg2 >> bit) & 0x1) << 2) |
				     (((reg1 >> bit) & 0x1) << 1) |
				     (((reg0 >> bit) & 0x1) << 0);

			const char *ca35 = (routing == 0) ? " O " : " - ";
			const char *ssp  = (routing == 1) ? " O " : " - ";
			const char *tsp  = (routing == 2) ? " O " : " - ";

			seq_printf(s, "%-4d| %s | %s | %s\n", idx, ca35, ssp, tsp);
		}
	}
	return 0;
}

static int aspeed_intc0_show_prot(struct seq_file *s, void *unused)
{
	struct aspeed_intc *intc = s->private;
	u32 prot = readl(intc->base + 0x40);

	seq_printf(s, "INTC040 : 0x%08x\n", prot);

	static const char * const prot_bits[] = {
		"hprot_ca35: Protect INTC010~018,1xxx accessed by PSP only",
		"hprot_ssp: Protect INTC020~028,2xxx accessed by SSP only",
		"hprot_tsp: Protect INTC030~038,3xxx accessed by TSP only",
		"hprot_sirqs: Protect INTC0C0~0D4 to be read only",
		"hprot_sirqs_1700: Protect INTC0D8~0DC to be read only",
		"hprot_sirqs_ext: Protect INTC0E0 to be read only",
		"hprot_reg_prot: Protect INTC044,2xx~3xx to be read only",
		"hprot_rd1_prot: Read protect for INTC044,200-438",
		"hprot_rd2_prot: Read protect for INTC0C0~164",
		"hprot_rd3_prot: Read protect for INTC02x,1xxx to be read by PSP only",
		"hprot_rd4_prot: Read protect for INTC03x,2xxx to be read by SSP only",
		"hprot_rd5_prot: Read protect for INTC04x,3xxx to be read by TSP only",
		"hprot_mcu0: Protect INTC050~054,028 accessed by MCU0 only",
		"hprot_ca35p: Protect INTC010~018 accessed by PSP secure only"
	};

	for (int i = 0; i < 14; i++)
		seq_printf(s, "  [%2d] %s: %s\n", i, prot_bits[i],
			   (prot & BIT(i)) ? "Enable" : "Disable");
	return 0;
}

/*
 * 000: Route interrupt INTi to PSP(default)
 * 001: Route interrupt INTi to INTC controller
 * 010: Route interrupt INTi to SSP
 * 011: Route interrupt INTi to TSP
 * 100: Route interrupt INTi to PSP S1
 * 101: Route interrupt INTi to PSP S2
 * 110: Route interrupt INTi to MCU0
 */
static int aspeed_intc1_show_routing(struct seq_file *s, void *unused)
{
	struct aspeed_intc *intc = s->private;
	int group, bit;

	seq_puts(s, "index      | PSP | INTC| SSP | TSP | S1  | S2  | MCU0\n");
	seq_puts(s, "-----------+-----+-----+-----+-----+-----+-----+-----\n");

	for (group = 0; group < 6; group++) {
		u32 reg0 = readl(intc->base + INTC1_ROUTING0_SEL0 + group * 4);
		u32 reg1 = readl(intc->base + INTC1_ROUTING0_SEL1 + group * 4);
		u32 reg2 = readl(intc->base + INTC1_ROUTING0_SEL2 + group * 4);

		for (bit = 0; bit < 32; bit++) {
			u8 routing = (((reg2 >> bit) & 0x1) << 2) |
				     (((reg1 >> bit) & 0x1) << 1) |
				     (((reg0 >> bit) & 0x1) << 0);

			const char *psp  = (routing == 0) ? " O " : " - ";
			const char *intc = (routing == 1) ? " O " : " - ";
			const char *ssp  = (routing == 2) ? " O " : " - ";
			const char *tsp  = (routing == 3) ? " O " : " - ";
			const char *s1   = (routing == 4) ? " O " : " - ";
			const char *s2   = (routing == 5) ? " O " : " - ";
			const char *mcu0 = (routing == 6) ? " O " : " - ";

			seq_printf(s, "intc1_%d_%02d | %s | %s | %s | %s | %s | %s | %s\n",
				   group, bit, psp, intc, ssp, tsp, s1, s2, mcu0);
		}
	}
	return 0;
}

static int aspeed_intc1_show_prot(struct seq_file *s, void *unused)
{
	struct aspeed_intc *intc = s->private;
	u32 prot = readl(intc->base);

	seq_printf(s, "INTC1: 0x%08x\n", prot);

	static const char * const prot_bits[] = {
		"pprot_ca35: Protect INTC100~150,280~2D0,300~350 write by PSP only",
		"pprot_ssp: Protect INTC180~1D0 write by SSP only",
		"pprot_tsp: Protect INTC200~250 write by TSP only",
		"pprot_reg_prot: Protect INTC080~0D4 to be read only",
		"pprot_regrd: Protect INTC080~0D4 to be read protected",
		"pprot_regrd2: Protect INTC100~150,280~2D0,300~350 read by PSP only",
		"pprot_regrd3: Protect INTC180~1D0 read by SSP only",
		"pprot_regrd4: Protect INTC200~250 read by TSP only",
		"pprot_mcu0: Protect INTC010,014 write by MCU0 only",
		"pprot_regrd5: Protect INTC010,014 read by MCU0 only",
		"pprot_treg: Protect INTC040~054 to be read protected"
	};

	for (int i = 0; i < 11; i++)
		seq_printf(s, "  [%2d] %s: %s\n", i, prot_bits[i],
			   (prot & BIT(i)) ? "Enable" : "Disable");
	return 0;
}

static int aspeed_intc_open_routing(struct inode *inode, struct file *file)
{
	struct aspeed_intc *intc = inode->i_private;

	if (!intc->show_routing)
		return -ENODEV;
	return single_open(file, intc->show_routing, intc);
}

static int aspeed_intc_open_prot(struct inode *inode, struct file *file)
{
	struct aspeed_intc *intc = inode->i_private;

	if (!intc->show_prot)
		return -ENODEV;
	return single_open(file, intc->show_prot, intc);
}

static const struct file_operations aspeed_intc_routing_fops = {
	.owner   = THIS_MODULE,
	.open    = aspeed_intc_open_routing,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static const struct file_operations aspeed_intc_prot_fops = {
	.owner   = THIS_MODULE,
	.open    = aspeed_intc_open_prot,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static int aspeed_intc_probe(struct platform_device *pdev)
{
	struct aspeed_intc *intc;
	struct resource *res;

	intc = devm_kzalloc(&pdev->dev, sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;
	intc->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	intc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(intc->base))
		return PTR_ERR(intc->base);

	if (of_device_is_compatible(pdev->dev.of_node, "aspeed,ast2700-intc0")) {
		intc->show_routing = aspeed_intc0_show_routing;
		intc->show_prot    = aspeed_intc0_show_prot;
	} else if (of_device_is_compatible(pdev->dev.of_node, "aspeed,ast2700-intc1")) {
		intc->show_routing = aspeed_intc1_show_routing;
		intc->show_prot    = aspeed_intc1_show_prot;
	} else {
		intc->show_routing = NULL;
		intc->show_prot = NULL;
	}

	platform_set_drvdata(pdev, intc);

	intc->dbg_root = debugfs_create_dir(dev_name(&pdev->dev), NULL);
	if (intc->dbg_root) {
		debugfs_create_file("routing", 0400, intc->dbg_root, intc,
				    &aspeed_intc_routing_fops);
		debugfs_create_file("protection", 0400, intc->dbg_root, intc,
				    &aspeed_intc_prot_fops);
	}

	return 0;
}

static const struct of_device_id aspeed_intc_of_match[] = {
	{ .compatible = "aspeed,ast2700-intc0", },
	{ .compatible = "aspeed,ast2700-intc1", },
	{},
};

static struct platform_driver aspeed_intc_driver = {
	.probe  = aspeed_intc_probe,
	.driver = {
		.name = "ast2700-intc",
		.of_match_table = aspeed_intc_of_match,
	},
};
builtin_platform_driver(aspeed_intc_driver);

