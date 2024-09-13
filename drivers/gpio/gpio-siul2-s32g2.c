// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SIUL2 GPIO support.
 *
 * Copyright (c) 2016 Freescale Semiconductor, Inc.
 * Copyright 2018-2024 NXP
  */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>
#include <linux/types.h>

/* PGPDOs are 16bit registers that come in big endian
 * order if they are grouped in pairs of two.
 *
 * For example, the order is PGPDO1, PGPDO0, PGPDO3, PGPDO2...
 */
#define SIUL2_PGPDO(N)		(((N) ^ 1) * 2)
#define S32G2_SIUL2_NUM		2
#define S32G2_PADS_DTS_TAG_LEN	(7)

#define SIUL2_GPIO_16_PAD_SIZE	16

/**
 * struct siul2_device_data  - platform data attached to the compatible.
 * @pad_access: access table for I/O pads, consists of S32G2_SIUL2_NUM tables.
 * @reset_cnt: reset the pin name counter to zero when switching to SIUL2_1.
 */
struct siul2_device_data {
	const struct regmap_access_table **pad_access;
	const bool reset_cnt;
};

/**
 * struct siul2_desc - describes a SIUL2 hw module.
 * @pad_access: array of valid I/O pads.
 * @opadmap: the regmap of the Parallel GPIO Pad Data Out Register.
 * @ipadmap: the regmap of the Parallel GPIO Pad Data In Register.
 * @gpio_base: the first GPIO pin.
 * @gpio_num: the number of GPIO pins.
 */
struct siul2_desc {
	const struct regmap_access_table *pad_access;
	struct regmap *opadmap;
	struct regmap *ipadmap;
	u32 gpio_base;
	u32 gpio_num;
};

/**
 * struct siul2_gpio_dev - describes a group of GPIO pins.
 * @platdata: the platform data.
 * @siul2: SIUL2_0 and SIUL2_1 modules information.
 * @pin_dir_bitmap: the bitmap with pin directions.
 * @gc: the GPIO chip.
 * @lock: mutual access to bitmaps.
 */
struct siul2_gpio_dev {
	const struct siul2_device_data *platdata;
	struct siul2_desc siul2[S32G2_SIUL2_NUM];
	unsigned long *pin_dir_bitmap;
	struct gpio_chip gc;
	raw_spinlock_t lock;
};

static const struct regmap_range s32g2_siul20_pad_yes_ranges[] = {
	regmap_reg_range(SIUL2_PGPDO(0), SIUL2_PGPDO(0)),
	regmap_reg_range(SIUL2_PGPDO(1), SIUL2_PGPDO(1)),
	regmap_reg_range(SIUL2_PGPDO(2), SIUL2_PGPDO(2)),
	regmap_reg_range(SIUL2_PGPDO(3), SIUL2_PGPDO(3)),
	regmap_reg_range(SIUL2_PGPDO(4), SIUL2_PGPDO(4)),
	regmap_reg_range(SIUL2_PGPDO(5), SIUL2_PGPDO(5)),
	regmap_reg_range(SIUL2_PGPDO(6), SIUL2_PGPDO(6)),
};

static const struct regmap_access_table s32g2_siul20_pad_access_table = {
	.yes_ranges	= s32g2_siul20_pad_yes_ranges,
	.n_yes_ranges	= ARRAY_SIZE(s32g2_siul20_pad_yes_ranges),
};

static const struct regmap_range s32g2_siul21_pad_yes_ranges[] = {
	regmap_reg_range(SIUL2_PGPDO(7), SIUL2_PGPDO(7)),
	regmap_reg_range(SIUL2_PGPDO(9), SIUL2_PGPDO(9)),
	regmap_reg_range(SIUL2_PGPDO(10), SIUL2_PGPDO(10)),
	regmap_reg_range(SIUL2_PGPDO(11), SIUL2_PGPDO(11)),
};

static const struct regmap_access_table s32g2_siul21_pad_access_table = {
	.yes_ranges	= s32g2_siul21_pad_yes_ranges,
	.n_yes_ranges	= ARRAY_SIZE(s32g2_siul21_pad_yes_ranges),
};

static const struct regmap_access_table *s32g2_pad_access_table[] = {
	&s32g2_siul20_pad_access_table,
	&s32g2_siul21_pad_access_table
};

static_assert(ARRAY_SIZE(s32g2_pad_access_table) == S32G2_SIUL2_NUM);

static const struct siul2_device_data s32g2_device_data = {
	.pad_access	= s32g2_pad_access_table,
	.reset_cnt	= true,
};

static int siul2_get_gpio_pinspec(struct platform_device *pdev,
				  struct of_phandle_args *pinspec,
				  unsigned int range_index)
{
	struct device_node *np = pdev->dev.of_node;

	return of_parse_phandle_with_fixed_args(np, "gpio-ranges", 3,
						range_index, pinspec);
}

static struct regmap *siul2_offset_to_regmap(struct siul2_gpio_dev *dev,
					     unsigned int offset,
					     bool input)
{
	struct siul2_desc *siul2;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(dev->siul2); i++) {
		siul2 = &dev->siul2[i];
		if (offset >= siul2->gpio_base &&
		    offset - siul2->gpio_base < siul2->gpio_num)
			return input ? siul2->ipadmap : siul2->opadmap;
	}

	return NULL;
}

static void siul2_gpio_set_direction(struct siul2_gpio_dev *dev,
				     unsigned int gpio, int dir)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&dev->lock, flags);

	if (dir == GPIO_LINE_DIRECTION_IN)
		__clear_bit(gpio, dev->pin_dir_bitmap);
	else
		__set_bit(gpio, dev->pin_dir_bitmap);

	raw_spin_unlock_irqrestore(&dev->lock, flags);
}


static int siul2_get_direction(struct siul2_gpio_dev *dev,
			       unsigned int gpio)
{
	return test_bit(gpio, dev->pin_dir_bitmap) ? GPIO_LINE_DIRECTION_OUT :
						     GPIO_LINE_DIRECTION_IN;
}

static struct siul2_gpio_dev *to_siul2_gpio_dev(struct gpio_chip *chip)
{
	return container_of(chip, struct siul2_gpio_dev, gc);
}

static int siul2_gpio_dir_in(struct gpio_chip *chip, unsigned int gpio)
{
	struct siul2_gpio_dev *gpio_dev;
	int ret = 0;

	ret = pinctrl_gpio_direction_input(chip, gpio);
	if (ret)
		return ret;

	gpio_dev = to_siul2_gpio_dev(chip);
	siul2_gpio_set_direction(gpio_dev, gpio, GPIO_LINE_DIRECTION_IN);

	return 0;
}

static int siul2_gpio_get_dir(struct gpio_chip *chip, unsigned int gpio)
{
	return siul2_get_direction(to_siul2_gpio_dev(chip), gpio);
}

static unsigned int siul2_pin2pad(unsigned int pin)
{
	return pin / SIUL2_GPIO_16_PAD_SIZE;
}

static u16 siul2_pin2mask(unsigned int pin)
{
	/**
	 * From Reference manual :
	 * PGPDOx[PPDOy] = GPDO(x × 16) + (15 - y)[PDO_(x × 16) + (15 - y)]
	 */
	return BIT(SIUL2_GPIO_16_PAD_SIZE - 1 - pin % SIUL2_GPIO_16_PAD_SIZE);
}

static void siul2_gpio_set_val(struct gpio_chip *chip, unsigned int offset,
			       int value)
{
	struct siul2_gpio_dev *gpio_dev = to_siul2_gpio_dev(chip);
	unsigned int pad, reg_offset;
	struct regmap *regmap;
	u16 mask;

	mask = siul2_pin2mask(offset);
	pad = siul2_pin2pad(offset);

	reg_offset = SIUL2_PGPDO(pad);
	regmap = siul2_offset_to_regmap(gpio_dev, offset, false);
	if (!regmap)
		return;

	value = value ? mask : 0;

	regmap_update_bits(regmap, reg_offset, mask, value);
}

static int siul2_gpio_dir_out(struct gpio_chip *chip, unsigned int gpio,
			      int val)
{
	struct siul2_gpio_dev *gpio_dev;
	int ret = 0;

	gpio_dev = to_siul2_gpio_dev(chip);
	siul2_gpio_set_val(chip, gpio, val);

	ret = pinctrl_gpio_direction_output(chip, gpio);
	if (ret)
		return ret;

	siul2_gpio_set_direction(gpio_dev, gpio, GPIO_LINE_DIRECTION_OUT);

	return 0;
}

static void siul2_gpio_set(struct gpio_chip *chip, unsigned int offset,
			   int value)
{
	struct siul2_gpio_dev *gpio_dev = to_siul2_gpio_dev(chip);

	if (!gpio_dev)
		return;

	if (siul2_get_direction(gpio_dev, offset) == GPIO_LINE_DIRECTION_IN)
		return;

	siul2_gpio_set_val(chip, offset, value);
}

static int siul2_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct siul2_gpio_dev *gpio_dev = to_siul2_gpio_dev(chip);
	unsigned int mask, pad, reg_offset, data = 0;
	struct regmap *regmap;

	mask = siul2_pin2mask(offset);
	pad = siul2_pin2pad(offset);

	reg_offset = SIUL2_PGPDO(pad);
	regmap = siul2_offset_to_regmap(gpio_dev, offset, true);
	if (!regmap)
		return -EINVAL;

	regmap_read(regmap, reg_offset, &data);

	return !!(data & mask);
}

static const struct regmap_config siul2_regmap_conf = {
	.val_bits = 32,
	.reg_bits = 32,
	.reg_stride = 4,
	.cache_type = REGCACHE_FLAT,
};

static struct regmap *common_regmap_init(struct platform_device *pdev,
					 struct regmap_config *conf,
					 const char *name)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	resource_size_t size;
	void __iomem *base;

	base = devm_platform_get_and_ioremap_resource_byname(pdev, name, &res);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Failed to get MEM resource: %s\n", name);
		return ERR_PTR(-EINVAL);
	}

	size = resource_size(res);
	conf->val_bits = conf->reg_stride * 8;
	conf->max_register = size - conf->reg_stride;
	conf->name = name;
	conf->use_raw_spinlock = true;

	if (conf->cache_type != REGCACHE_NONE)
		conf->num_reg_defaults_raw = size / conf->reg_stride;

	return devm_regmap_init_mmio(dev, base, conf);
}

static bool not_writable(__always_unused struct device *dev,
			 __always_unused unsigned int reg)
{
	return false;
}

static struct regmap *init_padregmap(struct platform_device *pdev,
				     struct siul2_gpio_dev *gpio_dev,
				     int selector, bool input)
{
	const struct siul2_device_data *platdata = gpio_dev->platdata;
	struct regmap_config regmap_conf = siul2_regmap_conf;
	char dts_tag[S32G2_PADS_DTS_TAG_LEN];
	int err;

	regmap_conf.reg_stride = 2;

	if (selector != 0 && selector != 1)
		return ERR_PTR(-EINVAL);

	regmap_conf.rd_table = platdata->pad_access[selector];

	err = snprintf(dts_tag, ARRAY_SIZE(dts_tag),  "%cpads%d",
		       input ? 'i' : 'o', selector);
	if (err < 0)
		return ERR_PTR(-EINVAL);

	if (input) {
		regmap_conf.writeable_reg = not_writable;
		regmap_conf.cache_type = REGCACHE_NONE;
	} else {
		regmap_conf.wr_table = platdata->pad_access[selector];
	}

	return common_regmap_init(pdev, &regmap_conf, dts_tag);
}

static int siul2_gpio_pads_init(struct platform_device *pdev,
				struct siul2_gpio_dev *gpio_dev)
{
	struct device *dev = &pdev->dev;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(gpio_dev->siul2); i++) {
		gpio_dev->siul2[i].opadmap = init_padregmap(pdev, gpio_dev, i,
							    false);
		if (IS_ERR(gpio_dev->siul2[i].opadmap)) {
			dev_err(dev,
				"Failed to initialize opad2%zu regmap config\n",
				i);
			return PTR_ERR(gpio_dev->siul2[i].opadmap);
		}

		gpio_dev->siul2[i].ipadmap = init_padregmap(pdev, gpio_dev, i,
							    true);
		if (IS_ERR(gpio_dev->siul2[i].ipadmap)) {
			dev_err(dev,
				"Failed to initialize ipad2%zu regmap config\n",
				i);
			return PTR_ERR(gpio_dev->siul2[i].ipadmap);
		}
	}

	return 0;
}

static int siul2_gen_names(struct device *dev, unsigned int cnt, char **names,
			   char *ch_index, unsigned int *num_index)
{
	unsigned int i;

	for (i = 0; i < cnt; i++) {
		if (i != 0 && !(*num_index % 16))
			(*ch_index)++;

		names[i] = devm_kasprintf(dev, GFP_KERNEL, "P%c_%02d",
					  *ch_index, 0xFU & (*num_index)++);
		if (!names[i])
			return -ENOMEM;
	}

	return 0;
}

static int siul2_gpio_remove_reserved_names(struct device *dev,
					    struct siul2_gpio_dev *gpio_dev,
					    char **names)
{
	struct device_node *np = dev->of_node;
	int num_ranges, i, j, ret;
	u32 base_gpio, num_gpio;

	/* Parse the gpio-reserved-ranges to know which GPIOs to exclude. */

	num_ranges = of_property_count_u32_elems(dev->of_node,
						 "gpio-reserved-ranges");

	/* The "gpio-reserved-ranges" is optional. */
	if (num_ranges < 0)
		return 0;
	num_ranges /= 2;

	for (i = 0; i < num_ranges; i++) {
		ret = of_property_read_u32_index(np, "gpio-reserved-ranges",
						 i * 2, &base_gpio);
		if (ret) {
			dev_err(dev, "Could not parse the start GPIO: %d\n",
				ret);
			return ret;
		}

		ret = of_property_read_u32_index(np, "gpio-reserved-ranges",
						 i * 2 + 1, &num_gpio);
		if (ret) {
			dev_err(dev, "Could not parse num. GPIOs: %d\n", ret);
			return ret;
		}

		if (base_gpio + num_gpio > gpio_dev->gc.ngpio) {
			dev_err(dev, "Reserved GPIOs outside of GPIO range\n");
			return -EINVAL;
		}

		/* Remove names set for reserved GPIOs. */
		for (j = base_gpio; j < base_gpio + num_gpio; j++) {
			devm_kfree(dev, names[j]);
			names[j] = NULL;
		}
	}

	return 0;
}

static int siul2_gpio_populate_names(struct device *dev,
				     struct siul2_gpio_dev *gpio_dev)
{
	unsigned int num_index = 0;
	char ch_index = 'A';
	char **names;
	int i, ret;

	names = devm_kcalloc(dev, gpio_dev->gc.ngpio, sizeof(*names),
			     GFP_KERNEL);
	if (!names)
		return -ENOMEM;

	for (i = 0; i < S32G2_SIUL2_NUM; i++) {
		ret = siul2_gen_names(dev, gpio_dev->siul2[i].gpio_num,
				      names + gpio_dev->siul2[i].gpio_base,
				      &ch_index, &num_index);
		if (ret) {
			dev_err(dev, "Could not set names for SIUL2_%d GPIOs\n",
				i);
			return ret;
		}

		if (gpio_dev->platdata->reset_cnt)
			num_index = 0;

		ch_index++;
	}

	ret = siul2_gpio_remove_reserved_names(dev, gpio_dev, names);
	if (ret)
		return ret;

	gpio_dev->gc.names = (const char *const *)names;

	return 0;
}

static int siul2_gpio_probe(struct platform_device *pdev)
{
	struct siul2_gpio_dev *gpio_dev;
	struct device *dev = &pdev->dev;
	struct of_phandle_args pinspec;
	size_t i, bitmap_size;
	struct gpio_chip *gc;
	int ret = 0;

	gpio_dev = devm_kzalloc(dev, sizeof(*gpio_dev), GFP_KERNEL);
	if (!gpio_dev)
		return -ENOMEM;

	gpio_dev->platdata = &s32g2_device_data;

	for (i = 0; i < S32G2_SIUL2_NUM; i++)
		gpio_dev->siul2[i].pad_access =
			gpio_dev->platdata->pad_access[i];

	ret = siul2_gpio_pads_init(pdev, gpio_dev);
	if (ret)
		return ret;

	gc = &gpio_dev->gc;

	platform_set_drvdata(pdev, gpio_dev);

	raw_spin_lock_init(&gpio_dev->lock);

	for (i = 0; i < ARRAY_SIZE(gpio_dev->siul2); i++) {
		ret = siul2_get_gpio_pinspec(pdev, &pinspec, i);
		if (ret) {
			dev_err(dev,
				"unable to get pinspec %zu from device tree\n",
				i);
			return -EINVAL;
		}

		of_node_put(pinspec.np);

		if (pinspec.args_count != 3) {
			dev_err(dev, "Invalid pinspec count: %d\n",
				pinspec.args_count);
			return -EINVAL;
		}

		gpio_dev->siul2[i].gpio_base = pinspec.args[1];
		gpio_dev->siul2[i].gpio_num = pinspec.args[2];
	}

	gc->base = -1;

	/* In some cases, there is a gap between the SIUL GPIOs. */
	gc->ngpio = gpio_dev->siul2[S32G2_SIUL2_NUM - 1].gpio_base +
		    gpio_dev->siul2[S32G2_SIUL2_NUM - 1].gpio_num;

	ret = siul2_gpio_populate_names(&pdev->dev, gpio_dev);
	if (ret)
		return ret;

	bitmap_size = BITS_TO_LONGS(gc->ngpio) *
		      sizeof(*gpio_dev->pin_dir_bitmap);
	gpio_dev->pin_dir_bitmap = devm_kzalloc(dev, bitmap_size, GFP_KERNEL);
	if (!gpio_dev->pin_dir_bitmap)
		return -ENOMEM;

	gc->parent = dev;
	gc->label = dev_name(dev);

	gc->set = siul2_gpio_set;
	gc->get = siul2_gpio_get;
	gc->set_config = gpiochip_generic_config;
	gc->request = gpiochip_generic_request;
	gc->free = gpiochip_generic_free;
	gc->direction_output = siul2_gpio_dir_out;
	gc->direction_input = siul2_gpio_dir_in;
	gc->get_direction = siul2_gpio_get_dir;
	gc->owner = THIS_MODULE;

	ret = devm_gpiochip_add_data(dev, gc, gpio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "unable to add gpiochip\n");

	return 0;
}

static const struct of_device_id siul2_gpio_dt_ids[] = {
	{ .compatible = "nxp,s32g2-siul2-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, siul2_gpio_dt_ids);

static struct platform_driver siul2_gpio_driver = {
	.driver			= {
		.name		= "s32g2-siul2-gpio",
		.of_match_table = siul2_gpio_dt_ids,
	},
	.probe			= siul2_gpio_probe,
};

module_platform_driver(siul2_gpio_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP SIUL2 GPIO");
MODULE_LICENSE("GPL");
