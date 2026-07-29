// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2026 Axiado Corporation
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/types.h>

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
	struct mutex output_lock;
	u32 ngpios;
	u32 max_sgpio_pins;
	u32 max_offset_regs;
	struct gpio_chip chip;
	u32 irq_unmasked[MAX_SGPIO_PINS];
	unsigned int irq_type[MAX_SGPIO_PINS];
	struct regmap *regmap;
	u32 regmap_base_offset;
	const struct sgpio_reg_offsets *regs;
};

struct axiado_sgpio_soc_data {
	const struct sgpio_reg_offsets *regs;
	u32 max_sgpio_pins;
	u32 max_offset_regs;
};

static const struct axiado_sgpio_soc_data ax3000_sgpio_data = {
	.regs = &sgpio_offsets_128,
	.max_sgpio_pins = 128,
	.max_offset_regs = 4,
};

static const struct axiado_sgpio_soc_data ax3005_sgpio_data = {
	.regs = &sgpio_offsets_512,
	.max_sgpio_pins = 512,
	.max_offset_regs = 16,
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
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int ax3000_sgpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	u32 position = (offset / 2) % 32;
	u32 bank = (offset / 2) / 32;
	u32 val;
	int ret;

	if (!(offset % 2))
		return -EINVAL;

	guard(mutex)(&sgpio->output_lock);

	val = sgpio->slices[2].reg_ss[bank];

	if (value)
		val |= BIT(position);
	else
		val &= ~BIT(position);

	ret = regmap_write(sgpio->regmap,
			   sgpio->regmap_base_offset + sgpio->regs->dout_ss +
			   bank * sizeof(u32), val);
	if (ret)
		return ret;

	sgpio->slices[2].reg_ss[bank] = val;

	return 0;
}

static int ax3000_sgpio_get_direction(struct gpio_chip *chip,
				      unsigned int offset)
{
	if (!(offset % 2))
		return GPIO_LINE_DIRECTION_IN;

	return GPIO_LINE_DIRECTION_OUT;
}

static int ax3000_sgpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	u32 position = (offset / 2) % 32;
	u32 bank = (offset / 2) / 32;
	u32 val;

	if (!(offset % 2)) {
		guard(spinlock_irqsave)(&sgpio->lock);

		val = sgpio->slices[3].reg_ss[bank];
	} else {
		guard(mutex)(&sgpio->output_lock);

		val = sgpio->slices[2].reg_ss[bank];
	}

	return !!(val & BIT(position));
}

static int ax3000_sgpio_dir_in(struct gpio_chip *chip, unsigned int offset)
{
	if (!(offset % 2))
		return 0;

	return -EINVAL;
}

static int ax3000_sgpio_dir_out(struct gpio_chip *chip, unsigned int offset,
				int value)
{
	/* Even offsets represent input GPIOs. */
	if (!(offset % 2))
		return -EINVAL;

	return ax3000_sgpio_set(chip, offset, value);
}

static irqreturn_t sgpio_irq_handler(int irq, void *arg)
{
	struct ax3000_sgpio *sgpio = (struct ax3000_sgpio *)arg;
	u32 status, new_value;
	u32 changed_value;
	int bit, reg_ptr;
	int ret, i;

	/* Read-on-clear (ACK) parent cause */
	ret = regmap_read(sgpio->regmap,
			  sgpio->regmap_base_offset + sgpio->regs->status,
			  &status);
	if (ret)
		return IRQ_NONE;

	status >>= 16;

	bool has_shifted_layout = (sgpio->max_offset_regs == MAX_OFFSET_REG);

	reg_ptr = has_shifted_layout ? 16 - DIV_ROUND_UP(sgpio->ngpios, 32) : 0;

	for (i = 0; i < DIV_ROUND_UP(sgpio->ngpios, 32); i++, reg_ptr++) {
		if (status & BIT(reg_ptr)) {
			ret = regmap_read(sgpio->regmap,
					  sgpio->regmap_base_offset +
					  sgpio->regs->din_ss +
					  reg_ptr * sizeof(u32),
					  &new_value);
			if (ret)
				continue;

			{
				guard(spinlock_irqsave)(&sgpio->lock);
				changed_value = sgpio->slices[3].reg_ss[i] ^ new_value;
				sgpio->slices[3].reg_ss[i] = new_value;
			}

			while (changed_value) {
				irq_hw_number_t hwirq;
				unsigned int position;
				unsigned int type;
				bool rising;

				bit = __ffs(changed_value);
				changed_value &= ~BIT(bit);

				position = i * 32 + bit;
				hwirq = position * 2;

				rising = !!(new_value & BIT(bit));
				type = READ_ONCE(sgpio->irq_type[position]);

				if (rising &&
				    !(type & IRQ_TYPE_EDGE_RISING))
					continue;

				if (!rising &&
				    !(type & IRQ_TYPE_EDGE_FALLING))
					continue;

				if (READ_ONCE(sgpio->irq_unmasked[position])) {
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
	bool has_shifted_layout = (sgpio->max_offset_regs == MAX_OFFSET_REG);
	u32 position;
	u32 bank;
	int i;

	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->mask, 0);

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
		val |= GENMASK(position, 0);

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
		     sgpio->regmap_base_offset + sgpio->regs->ctrl_en, 0xffff);
	regmap_write(sgpio->regmap,
		     sgpio->regmap_base_offset + sgpio->regs->ctrl_en_pos,
		     0xffff);
}

static int axiado_sgpio_irq_enable(struct ax3000_sgpio *sgpio)
{
	return regmap_write(sgpio->regmap,
			    sgpio->regmap_base_offset + sgpio->regs->mask,
			    0xdfff);
}

static int sgpio_set_irq_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	unsigned int position;

	/* Only even GPIO offsets represent SGPIO inputs. */
	if (hwirq & 1)
		return -EINVAL;

	position = hwirq / 2;
	if (position >= sgpio->ngpios)
		return -EINVAL;

	type &= IRQ_TYPE_SENSE_MASK;

	switch (type) {
	case IRQ_TYPE_EDGE_BOTH:
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		break;
	default:
		return -EINVAL;
	}

	WRITE_ONCE(sgpio->irq_type[position], type);
	irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static void sgpio_mask_irq(struct irq_data *d)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	WRITE_ONCE(sgpio->irq_unmasked[hwirq / 2], 0);
	gpiochip_disable_irq(chip, hwirq);
}

static void sgpio_unmask_irq(struct irq_data *d)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct ax3000_sgpio *sgpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_enable_irq(chip, hwirq);
	WRITE_ONCE(sgpio->irq_unmasked[hwirq / 2], 1);
}

static void sgpio_irq_shutdown(struct irq_data *d)
{
	sgpio_mask_irq(d);
}

static void axiado_sgpio_irq_init_valid_mask(struct gpio_chip *chip,
					 unsigned long *valid_mask,
					 unsigned int ngpios)
{
	unsigned int offset;

	/* Only even offsets represent input GPIOs. */
	for (offset = 1; offset < ngpios; offset += 2)
		clear_bit(offset, valid_mask);
}

static int axiado_sgpio_init_input_cache(struct ax3000_sgpio *sgpio)
{
	unsigned int num_banks = DIV_ROUND_UP(sgpio->ngpios, 32);
	unsigned int reg_ptr;
	unsigned int i;
	int ret;

	reg_ptr = sgpio->max_offset_regs == MAX_OFFSET_REG ?
		  MAX_OFFSET_REG - num_banks : 0;

	for (i = 0; i < num_banks; i++, reg_ptr++) {
		ret = regmap_read(sgpio->regmap,
				  sgpio->regmap_base_offset +
				  sgpio->regs->din_ss +
				  reg_ptr * sizeof(u32),
				  &sgpio->slices[3].reg_ss[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct regmap_config regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int sgpio_probe(struct platform_device *pdev)
{
	const struct axiado_sgpio_soc_data *soc_data;
	struct gpio_irq_chip *girq;
	struct ax3000_sgpio *sgpio;
	unsigned long apb_freq;
	struct clk *apb_clk;
	void __iomem *base;
	u32 sgpio_freq;
	int irq, rc;
	u32 pos;

	sgpio = devm_kzalloc(&pdev->dev, sizeof(*sgpio), GFP_KERNEL);
	if (!sgpio)
		return -ENOMEM;

	spin_lock_init(&sgpio->lock);
	mutex_init(&sgpio->output_lock);

	sgpio->regmap = dev_get_regmap(pdev->dev.parent, NULL);

	if (sgpio->regmap) {
		rc = device_property_read_u32(&pdev->dev, "reg",
					      &sgpio->regmap_base_offset);
		if (rc)
			return dev_err_probe(&pdev->dev, rc,
					     "Failed to read reg property\n");

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
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "Failed to read ngpios property\n");

	soc_data = device_get_match_data(&pdev->dev);
	if (!soc_data)
		return -EINVAL;

	sgpio->regs = soc_data->regs;
	sgpio->max_sgpio_pins = soc_data->max_sgpio_pins;
	sgpio->max_offset_regs = soc_data->max_offset_regs;

	if (!sgpio->ngpios ||
	    (sgpio->ngpios != 128 && sgpio->ngpios != 512) ||
	    sgpio->ngpios > sgpio->max_sgpio_pins)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Invalid ngpios value: %u\n",
				     sgpio->ngpios);

	apb_clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(apb_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(apb_clk),
				     "Failed to get and enable APB clock\n");

	rc = device_property_read_u32(&pdev->dev, "bus-frequency",
				      &sgpio_freq);
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "Failed to read bus-frequency\n");

	apb_freq = clk_get_rate(apb_clk);

	if (!apb_freq || !sgpio_freq || sgpio_freq > apb_freq)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Invalid SGPIO bus frequency\n");

	sgpio->preset_value = (apb_freq / sgpio_freq) - 1;
	sgpio->count_value = sgpio->preset_value;

	pos = sgpio->ngpios - 1;
	sgpio->pos_reg = FIELD_PREP(GENMASK(31, 16), pos) |
			 FIELD_PREP(GENMASK(15, 0), pos);

	sgpio_hw_init(sgpio);

	rc = axiado_sgpio_init_input_cache(sgpio);
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "Failed to initialize input cache\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	rc = devm_request_threaded_irq(&pdev->dev, irq, NULL, sgpio_irq_handler,
				       IRQF_ONESHOT, dev_name(&pdev->dev), sgpio);

	if (rc)
		return dev_err_probe(&pdev->dev, rc, "Failed to request IRQ\n");

	sgpio->chip.parent = &pdev->dev;
	sgpio->chip.ngpio = sgpio->ngpios * 2;
	sgpio->chip.get_direction = ax3000_sgpio_get_direction;
	sgpio->chip.direction_input = ax3000_sgpio_dir_in;
	sgpio->chip.direction_output = ax3000_sgpio_dir_out;
	sgpio->chip.get = ax3000_sgpio_get;
	sgpio->chip.set = ax3000_sgpio_set;
	sgpio->chip.can_sleep = true;
	sgpio->chip.label = dev_name(&pdev->dev);
	sgpio->chip.base = -1;

	girq = &sgpio->chip.irq;

	gpio_irq_chip_set_chip(girq, &axiado_sgpio_irqchip);
	girq->handler = handle_bad_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->init_valid_mask = axiado_sgpio_irq_init_valid_mask;
	girq->threaded = true;
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;

	rc = devm_gpiochip_add_data(&pdev->dev, &sgpio->chip, sgpio);
	if (rc < 0)
		return dev_err_probe(&pdev->dev, rc,
				     "Could not register gpiochip\n");

	rc = axiado_sgpio_irq_enable(sgpio);
	if (rc)
		return dev_err_probe(&pdev->dev, rc,
				     "Failed to enable interrupts\n");

	platform_set_drvdata(pdev, sgpio);

	return 0;
}

static void sgpio_remove(struct platform_device *pdev)
{
	struct ax3000_sgpio *sgpio = platform_get_drvdata(pdev);

	/* Disable interrupts in hardware */
	if (sgpio->regs) {
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->mask,
			     0x0);
		regmap_write(sgpio->regmap,
			     sgpio->regmap_base_offset + sgpio->regs->ctrl_en,
			     0x0);
	}
}

static const struct of_device_id axiado_sgpio_of_match[] = {
	{ .compatible = "axiado,ax3000-sgpio", .data = &ax3000_sgpio_data },
	{ .compatible = "axiado,ax3005-sgpio", .data = &ax3005_sgpio_data },
	{ }
};
MODULE_DEVICE_TABLE(of, axiado_sgpio_of_match);

static struct platform_driver sgpio_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = axiado_sgpio_of_match,
	},
	.probe = sgpio_probe,
	.remove = sgpio_remove,
};

module_platform_driver(sgpio_driver);
MODULE_DESCRIPTION("Axiado Serial GPIO Driver");
MODULE_AUTHOR("Axiado Corporation");
MODULE_LICENSE("GPL");
