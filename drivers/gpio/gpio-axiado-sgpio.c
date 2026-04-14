// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2026 Axiado Corporation
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>

#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>

#include <linux/gpio/driver.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>

#include <linux/regmap.h>

struct sgpio_reg_offsets {
	u32 mux_0;
	u32 preset_0;
	u32 count_0;
	u32 pos_0;

	u32 mux_1;
	u32 ld;
	u32 ld_ss;

	u32 preset_1;
	u32 count_1;
	u32 pos_1;

	u32 mux_2;
	u32 dout;
	u32 dout_ss;

	u32 preset_2;
	u32 count_2;
	u32 pos_2;

	u32 mux_3;
	u32 preset_3;
	u32 count_3;
	u32 pos_3;

	u32 mux_4;
	u32 oe;
	u32 oe_ss;

	u32 preset_4;
	u32 count_4;
	u32 pos_4;

	u32 mask;
	u32 ctrl_en;
	u32 ctrl_en_pos;

	u32 din_ss;
	u32 status;
};

static const struct sgpio_reg_offsets sgpio_offsets_512 = {
	.mux_0 = 0x000,
	.preset_0 = 0x1dc,
	.count_0 = 0x1f0,
	.pos_0 = 0x204,

	.mux_1 = 0x004,
	.ld = 0x014,
	.ld_ss = 0x0d8,

	.preset_1 = 0x1e0,
	.count_1 = 0x1f4,
	.pos_1 = 0x208,

	.mux_2 = 0x008,
	.dout = 0x054,
	.dout_ss = 0x158,

	.preset_2 = 0x1e4,
	.count_2 = 0x1f8,
	.pos_2 = 0x20c,

	.mux_3 = 0x00c,
	.preset_3 = 0x1e8,
	.count_3 = 0x1fc,
	.pos_3 = 0x210,

	.mux_4 = 0x010,
	.oe = 0x0d4,
	.oe_ss = 0x1d8,

	.preset_4 = 0x1ec,
	.count_4 = 0x200,
	.pos_4 = 0x214,

	.mask = 0x224,
	.ctrl_en = 0x218,
	.ctrl_en_pos = 0x21c,

	.din_ss = 0x198,
	.status = 0x228,
};

static const struct sgpio_reg_offsets sgpio_offsets_128 = {
	.mux_0 = 0x000,
	.preset_0 = 0x08c,
	.count_0 = 0x0a0,
	.pos_0 = 0x0b4,

	.mux_1 = 0x004,
	.ld = 0x014,
	.ld_ss = 0x048,

	.preset_1 = 0x090,
	.count_1 = 0x0a4,
	.pos_1 = 0x0b8,

	.mux_2 = 0x008,
	.dout = 0x024,
	.dout_ss = 0x068,

	.preset_2 = 0x094,
	.count_2 = 0x0a8,
	.pos_2 = 0x0bc,

	.mux_3 = 0x00c,
	.preset_3 = 0x098,
	.count_3 = 0x0ac,
	.pos_3 = 0x0c0,

	.mux_4 = 0x010,
	.oe = 0x044,
	.oe_ss = 0x088,

	.preset_4 = 0x09c,
	.count_4 = 0x0b0,
	.pos_4 = 0x0c4,

	.mask = 0x0d4,
	.ctrl_en = 0x0c8,
	.ctrl_en_pos = 0x0cc,

	.din_ss = 0x078,
	.status = 0x0d8,
};

#define MAX_SGPIO_PINS 512
#define MAX_OFFSET_REG 16
#define MAX_SLICE_COUNT 5

struct ax3000_slice_info {
	u32 out_mux;
	u32 sgpio_mux;
	u32 slice_mux;
	u32 reg[MAX_OFFSET_REG];
	u32 reg_ss[MAX_OFFSET_REG];
	u32 preset;
	u32 count;
	u32 pos;
};

struct ax3000_sgpio {
	u32 preset_value;
	u32 count_value;
	u32 pos_reg;
	struct ax3000_slice_info
		slices[MAX_SLICE_COUNT]; /* 0=clk,1=load,2=out,3=in,4=oe */
	spinlock_t lock;
	int ngpios;
	int max_sgpio_pins;
	int max_offset_regs;
	struct gpio_chip chip;
	u32 irq_unmasked[MAX_SGPIO_PINS];
	int parent_irq;
	struct regmap *regmap;
	u32 regmap_base_offset;
	struct sgpio_reg_offsets *regs;
};

static int sgpio_set_irq_type(struct irq_data *d, unsigned int type);
static void sgpio_mask_irq(struct irq_data *d);
static void sgpio_unmask_irq(struct irq_data *d);
static void sgpio_irq_shutdown(struct irq_data *d);

static const struct irq_chip axiado_sgpio_irqchip = {
	.name = "axiado-sgpio",
	.irq_mask = sgpio_mask_irq,
	.irq_unmask = sgpio_unmask_irq,
	.irq_set_type = sgpio_set_irq_type,
	.irq_shutdown = sgpio_irq_shutdown,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_MASK_ON_SUSPEND,
};

static void ax3000_sgpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	unsigned long flags;
	u32 bank = (offset / 2) / 32;
	u32 position = (offset / 2) % 32;

	spin_lock_irqsave(&sgpio->lock, flags);
	if (value)
		sgpio->slices[2].reg_ss[bank] |= BIT(position);
	else
		sgpio->slices[2].reg_ss[bank] &= ~BIT(position);

	spin_unlock_irqrestore(&sgpio->lock, flags);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->dout_ss +
			     (bank * 4),
		     sgpio->slices[2].reg_ss[bank]);
}

static int ax3000_sgpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	u32 bank = (offset / 2) / 32;
	u32 position = (offset / 2) % 32;

	if (offset % 2 == 0)
		return !!(sgpio->slices[3].reg_ss[bank] & BIT(position));
	else
		return !!(sgpio->slices[2].reg_ss[bank] & BIT(position));
}

static int ax3000_sgpio_dir_in(struct gpio_chip *chip, unsigned int offset)
{
	if (!(offset % 2))
		return 0;
	else
		return -EINVAL;
}

static int ax3000_sgpio_dir_out(struct gpio_chip *chip, unsigned int offset,
				int value)
{
	if (offset % 2) {
		if (chip->set)
			chip->set(chip, offset, value);
		return 0;
	} else {
		return -EINVAL;
	}
}

static irqreturn_t sgpio_irq_handler(int irq, void *arg)
{
	struct ax3000_sgpio *sgpio = (struct ax3000_sgpio *)arg;
	unsigned long flags;
	u32 status, new_value;
	u32 changed_value;
	int i, bit, reg_ptr;

	/* Read-on-clear (ACK) parent cause */
	regmap_read(sgpio->regmap,
		    sgpio->regmap_base_offset + sgpio->regs->status, &status);
	status >>= 16;

	bool has_shifted_layout = (sgpio->max_offset_regs == MAX_OFFSET_REG);

	reg_ptr = has_shifted_layout ? 16 - DIV_ROUND_UP(sgpio->ngpios, 32) : 0;

	for (i = 0; i < DIV_ROUND_UP(sgpio->ngpios, 32); i++, reg_ptr++) {
		if (status & BIT(reg_ptr)) {
			regmap_read(sgpio->regmap,
				    sgpio->regmap_base_offset +
					    sgpio->regs->din_ss + (reg_ptr * 4),
				    &new_value);
			spin_lock_irqsave(&sgpio->lock, flags);
			changed_value = sgpio->slices[3].reg_ss[i] ^ new_value;
			sgpio->slices[3].reg_ss[i] = new_value;
			spin_unlock_irqrestore(&sgpio->lock, flags);

			while (changed_value) {
				bit = __ffs(changed_value);
				changed_value &= ~BIT(bit);

				irq_hw_number_t hwirq = i * 32 + bit;

				if (sgpio->irq_unmasked[hwirq]) {
					unsigned int child_irq;

					child_irq = irq_find_mapping(sgpio->chip.irq.domain,
								     hwirq);

					if (child_irq)
						handle_nested_irq(child_irq);
				}
			}
		}
	}

	return IRQ_HANDLED;
}

static void sgpio_hw_init(struct ax3000_sgpio *sgpio)
{
	u32 bank;
	u32 position;
	int i = 0;
	bool has_shifted_layout = (sgpio->max_offset_regs == MAX_OFFSET_REG);

	/* slice A0, Clock Pin - 0 */
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mux_0, 0x306);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->preset_0,
		     sgpio->preset_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->count_0,
		     sgpio->count_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->pos_0, 0x1f001f);

	/* Slice B1, Data Load Pin - 1 */
	bank = (sgpio->ngpios - 1) / 32;
	position = (sgpio->ngpios - 1) % 32;

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mux_1,
		     has_shifted_layout ? 0x30c : 0x304);

	for (i = 0; i < bank; i++) {
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ld +
				     (i * 4),
			     0xffffffff);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ld_ss +
				     (i * 4),
			     0xffffffff);
	}

	if (position) {
		u32 val;

		val = sgpio->slices[1].reg_ss[i];
		val |= GENMASK(position - 1, 0);

		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ld +
				     (i * 4),
			     val);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ld_ss +
				     (i * 4),
			     val);
	}

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->preset_1,
		     sgpio->preset_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->count_1,
		     sgpio->count_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->pos_1,
		     sgpio->pos_reg);

	/* Slice C2, Data Out Pin - 2 */
	bank = sgpio->ngpios / 32;
	position = sgpio->ngpios % 32;

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mux_2,
		     has_shifted_layout ? 0x30c : 0x304);

	for (i = 0; i < bank; i++) {
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->dout +
				     (i * 4),
			     sgpio->slices[2].reg_ss[i]);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->dout_ss +
				     (i * 4),
			     sgpio->slices[2].reg_ss[i]);
	}

	if (position) {
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->dout +
				     (i * 4),
			     sgpio->slices[2].reg_ss[i]);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->dout_ss +
				     (i * 4),
			     sgpio->slices[2].reg_ss[i]);
	}

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->preset_2,
		     sgpio->preset_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->count_2,
		     sgpio->count_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->pos_2,
		     sgpio->pos_reg);

	/* Slice D3, Data In Pin - 3 */
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mux_3, 0x14C);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->preset_3,
		     sgpio->preset_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->count_3,
		     sgpio->count_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->pos_3,
		     sgpio->pos_reg);

	/* Slice E4, Output Enable for respective pins */
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mux_4,
		     has_shifted_layout ? 0x10c : 0x104);
	regmap_write(sgpio->regmap, sgpio->regmap_base_offset + sgpio->regs->oe,
		     0xffffffff);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->oe_ss,
		     0xffffffff);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->preset_4,
		     sgpio->preset_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->count_4,
		     sgpio->count_value);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->pos_4, 0x1f001f);

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mask, 0xdfff);

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->ctrl_en, 0xffff);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->ctrl_en_pos,
		     0xffff);
}

static int sgpio_set_irq_type(struct irq_data *d, unsigned int type)
{
	switch (type) {
	case IRQ_TYPE_EDGE_BOTH:
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void sgpio_mask_irq(struct irq_data *d)
{
	struct gpio_chip *chip;
	struct ax3000_sgpio *sgpio;
	u32 irq_num;

	chip = irq_data_get_irq_chip_data(d);
	if (!chip) {
		pr_err("Unable to get gpio_chip for IRQ\n");
		return;
	}

	sgpio = gpiochip_get_data(chip);
	if (!sgpio) {
		pr_err("Unable to get chip data\n");
		return;
	}

	irq_num = irqd_to_hwirq(d);
	sgpio->irq_unmasked[irq_num / 2] = 0;
}

static void sgpio_unmask_irq(struct irq_data *d)
{
	struct gpio_chip *chip;
	struct ax3000_sgpio *sgpio;
	u32 irq_num;

	chip = irq_data_get_irq_chip_data(d);
	if (!chip) {
		pr_err("Unable to get gpio_chip for IRQ\n");
		return;
	}

	sgpio = gpiochip_get_data(chip);
	if (!sgpio) {
		pr_err("Unable to get chip data\n");
		return;
	}

	irq_num = irqd_to_hwirq(d);
	sgpio->irq_unmasked[irq_num / 2] = 1;
}

static void sgpio_irq_shutdown(struct irq_data *d)
{
	sgpio_mask_irq(d);
}

static int sgpio_probe(struct platform_device *pdev)
{
	int rc;
	int irq;
	int i;
	const __be32 *prop;
	struct gpio_irq_chip *girq;
	struct ax3000_sgpio *sgpio;
	u32 variant;
	u32 dout_value;
	u32 bus_frequency;
	u32 apb_frequency;
	int dout_reverse;

	void __iomem *base;

	const struct regmap_config regmap_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};

	sgpio = devm_kzalloc(&pdev->dev, sizeof(*sgpio), GFP_KERNEL);
	if (!sgpio)
		return -ENOMEM;

	spin_lock_init(&sgpio->lock);

	sgpio->regmap = dev_get_regmap(pdev->dev.parent, NULL);

	if (sgpio->regmap) {
		rc = of_property_read_u32(pdev->dev.of_node, "reg",
					  &sgpio->regmap_base_offset);
		if (rc) {
			dev_err(&pdev->dev, "Failed to read reg property: %d\n",
				rc);
			return rc;
		}
		dev_info(&pdev->dev, "Using regmap with base offset: 0x%x\n",
			 sgpio->regmap_base_offset);
	} else {
		base = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(base))
			return PTR_ERR(base);

		sgpio->regmap =
			devm_regmap_init_mmio(&pdev->dev, base, &regmap_config);

		if (IS_ERR(sgpio->regmap))
			return PTR_ERR(sgpio->regmap);

		sgpio->regmap_base_offset = 0;

		dev_info(&pdev->dev, "Using MMIO regmap\n");
	}

	rc = device_property_read_u32(&pdev->dev, "ngpios", &sgpio->ngpios);
	if (rc < 0) {
		dev_err(&pdev->dev, "Could not read ngpios property\n");
		return -EINVAL;
	}

	if (device_property_read_u32(&pdev->dev, "design-variant", &variant)) {
		dev_err(&pdev->dev, "design-variant not specified in DT\n");
		return -EINVAL;
	}

	if (variant == 128) {
		sgpio->regs = &sgpio_offsets_128;
		sgpio->max_sgpio_pins = 128;
		sgpio->max_offset_regs = 4;
	} else if (variant == 512) {
		sgpio->regs = &sgpio_offsets_512;
		sgpio->max_sgpio_pins = 512;
		sgpio->max_offset_regs = 16;
	} else {
		return -EINVAL;
	}

	if (sgpio->ngpios > sgpio->max_sgpio_pins) {
		dev_err(&pdev->dev, "ngpio is greater than 512 pins\n");
		return -EINVAL;
	}

	rc = device_property_read_u32(&pdev->dev, "bus-frequency",
				      &bus_frequency);
	if (rc < 0) {
		dev_err(&pdev->dev, "Could not read bus-frequency property\n");
		return -EINVAL;
	}

	rc = device_property_read_u32(&pdev->dev, "apb-frequency",
				      &apb_frequency);
	if (rc < 0) {
		dev_err(&pdev->dev, "Could not read apb-frequency property\n");
		return -EINVAL;
	}

	sgpio->preset_value = (apb_frequency / bus_frequency) - 1;
	sgpio->count_value = sgpio->preset_value;

	u32 pos;

	pos = sgpio->ngpios - 1;
	sgpio->pos_reg = (pos << 16) | pos;

	prop = of_get_property(pdev->dev.of_node, "dout-init", NULL);
	if (!prop) {
		dev_err(&pdev->dev, "Failed to get dout-init\n");
		return -EINVAL;
	}

	for (i = 0; i < sgpio->max_offset_regs; i++) {
		sgpio->slices[2].reg_ss[i] = 0;
		dout_value = be32_to_cpu(prop[i]);

		for (dout_reverse = 0; dout_reverse < 32; ++dout_reverse) {
			sgpio->slices[2].reg_ss[i] <<= 1;
			sgpio->slices[2].reg_ss[i] |= (dout_value & 1);
			dout_value >>= 1;
		}
	}

	sgpio_hw_init(sgpio);

	irq = platform_get_irq(pdev, 0);

	if (irq < 0) {
		dev_err(&pdev->dev, "Failed to get parent IRQ: %d\n", irq);
		return irq;
	}
	/* Store parent IRQ for cleanup */
	sgpio->parent_irq = irq;

	rc = devm_request_threaded_irq(&pdev->dev, irq, NULL, sgpio_irq_handler,
				       IRQF_ONESHOT, "axiado-sgpio", sgpio);

	if (rc < 0) {
		dev_err(&pdev->dev, "Failed to request threaded IRQ %d: %d\n",
			irq, rc);
		return rc;
	}

	sgpio->chip.parent = &pdev->dev;
	sgpio->chip.ngpio = sgpio->ngpios * 2;
	sgpio->chip.owner = THIS_MODULE;
	sgpio->chip.direction_input = ax3000_sgpio_dir_in;
	sgpio->chip.direction_output = ax3000_sgpio_dir_out;
	sgpio->chip.get = ax3000_sgpio_get;
	sgpio->chip.set = ax3000_sgpio_set;
	sgpio->chip.label = dev_name(&pdev->dev);
	sgpio->chip.base = -1;

	girq = &sgpio->chip.irq;

	girq->chip = &axiado_sgpio_irqchip;
	girq->handler = handle_edge_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->num_parents = 1;
	girq->parents =
		devm_kcalloc(&pdev->dev, 1, sizeof(*girq->parents), GFP_KERNEL);
	if (!girq->parents) {
		dev_err(&pdev->dev, "Failed to allocate parents array\n");
		return -ENOMEM;
	}
	girq->parents[0] = irq;

	rc = devm_gpiochip_add_data(&pdev->dev, &sgpio->chip, sgpio);
	if (rc < 0) {
		dev_err(&pdev->dev, "Could not register gpiochip, %d\n", rc);
		return rc;
	}

	/* Store driver data for remove() */
	platform_set_drvdata(pdev, sgpio);
	dev_info(&pdev->dev, "SGPIO registered with %d GPIOs\n",
		 sgpio->chip.ngpio);

	return 0;
}

static int sgpio_remove(struct platform_device *pdev)
{
	struct ax3000_sgpio *sgpio = platform_get_drvdata(pdev);
	int i;

	if (!sgpio)
		return 0;

	/* Disable interrupts in hardware */
	if (sgpio->regs) {
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->mask,
			     0x0);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ctrl_en,
			     0x0);
	}

	/* Disable and synchronize parent IRQ to avoid races with handlers */
	if (sgpio->parent_irq >= 0) {
		disable_irq(sgpio->parent_irq);
		synchronize_irq(sgpio->parent_irq);
	}

	/* Ensure all GPIO IRQ handlers complete before removal */
	if (sgpio->chip.irq.domain) {
		struct irq_domain *domain = sgpio->chip.irq.domain;
		unsigned int irq;
		int hwirq;

		for (hwirq = 0; hwirq < sgpio->chip.ngpio; hwirq++) {
			irq = irq_find_mapping(domain, hwirq);
			if (irq) {
				disable_irq(irq);
				synchronize_irq(irq);
			}
		}
	}

	/* Clear internal IRQ state */
	for (i = 0; i < sgpio->max_sgpio_pins; i++)
		sgpio->irq_unmasked[i] = 0;

	return 0;
}

static const struct of_device_id ax_sgpio_match[] = {
	{ .compatible = "axiado,sgpio" },
	{}
};
MODULE_DEVICE_TABLE(of, ax_sgpio_match);

static struct platform_driver sgpio_driver = {
	.driver = {
		.name = "sgpio",
		.owner = THIS_MODULE,
		.of_match_table = ax_sgpio_match,
	},
	.probe = sgpio_probe,
	.remove = sgpio_remove,
};

static int __init ax_sgpio_init(void)
{
	int ret;

	ret = platform_driver_register(&sgpio_driver);
	if (ret < 0) {
		pr_err("Failed to register SGPIO driver\n");
		return ret;
	}

	return 0;
}

static void __exit ax_sgpio_exit(void)
{
	platform_driver_unregister(&sgpio_driver);
}

module_init(ax_sgpio_init);
module_exit(ax_sgpio_exit);

MODULE_DESCRIPTION("Axiado Serial GPIO Driver");
MODULE_AUTHOR("Axiado Corporation");
MODULE_LICENSE("GPL");
