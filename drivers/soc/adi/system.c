// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2022-2024 - Analog Devices Inc.
 */

#include <linux/module.h>
#include <linux/soc/adi/system_config.h>

#define ADI_SYSREG_BITS(_id, _offset, _width, _shift) \
	{ \
		.id = ADI_SYSTEM_REG_##_id, \
		.offset = _offset, \
		.mask = GENMASK(_width-1, 0) << _shift, \
		.shift = _shift, \
		.is_bits = true, \
	}

#define ADI_SYSREG(_id, _offset) \
	{ \
		.id = ADI_SYSTEM_REG_##_id, \
		.offset = _offset, \
		.is_bits = false, \
	}

#define devm_regmap_init_adi_system_config(dev, config) \
	__regmap_lockdep_wrapper(__devm_regmap_init_adi_system_config, \
	#config, dev, config)

struct adi_system_context {
	/* underlying regmap_mmio */
	struct regmap *regmap;
	/* tree of register definitions by index */
	struct radix_tree_root tree;
	/* configuration we were created with */
	struct adi_system_config *config;
};

/*
 * Fields in PADS CFG0 at offset +0x04
 */
static struct adi_system_register adi_pads_regs[] = {
	ADI_SYSREG_BITS(EMAC0_PTPCLK0, 0x04, 2, 0),
	ADI_SYSREG_BITS(EMAC0_EMACRESET, 0x04, 1, 2),
	ADI_SYSREG_BITS(EMAC0_PHYISEL, 0x04, 2, 3),
	ADI_SYSREG_BITS(CNT0UDSEL, 0x04, 2, 6),
	ADI_SYSREG_BITS(CNT0DGSEL, 0x04, 2, 7),
	ADI_SYSREG_BITS(PUTMS, 0x04, 2, 16),
	ADI_SYSREG_BITS(EMAC0_AUXIE, 0x04, 1, 17),
	ADI_SYSREG_BITS(FAULT_DIS, 0x04, 1, 18),
	ADI_SYSREG_BITS(EMAC0_ENDIANNESS, 0x04, 1, 19),
	ADI_SYSREG_BITS(EMAC1_ENDIANNESS, 0x04, 1, 20),
	ADI_SYSREG_BITS(MSHC_CCLK_DIV_EN, 0x04, 1, 22),
	ADI_SYSREG(DAI0_IE, 0x90),
	ADI_SYSREG(DAI1_IE, 0x94),
};

static struct adi_system_config adi_pads_config = {
	.registers = adi_pads_regs,
	.len = ARRAY_SIZE(adi_pads_regs),
	.max_register = __ADI_SYSTEM_REG_COUNT,
};

static int regmap_system_read(void *context, unsigned int reg,
		unsigned int *val)
{
	struct adi_system_context *ctx = context;
	struct adi_system_register *sreg =
		radix_tree_lookup(&ctx->tree, reg);
	int ret;

	if (!sreg)
		return -EIO;

	if (sreg->is_bits) {
		uint32_t tmp;

		ret = regmap_read(ctx->regmap, sreg->offset, &tmp);
		if (ret)
			return ret;

		tmp = (tmp & sreg->mask) >> sreg->shift;
		*val = tmp;
		return 0;
	}

	return regmap_read(ctx->regmap, sreg->offset, val);
}

static int regmap_system_write(void *context, unsigned int reg,
		unsigned int val)
{
	struct adi_system_context *ctx = context;
	struct adi_system_register *sreg = radix_tree_lookup(&ctx->tree, reg);

	if (!sreg)
		return -EIO;

	if (sreg->is_bits) {
		return regmap_update_bits(ctx->regmap, sreg->offset,
				sreg->mask,
			(val << sreg->shift) & sreg->mask);
	}

	return regmap_write(ctx->regmap, sreg->offset, val);
}

static struct adi_system_context *create_context
(struct adi_system_config *config)
{
	struct regmap *regmap = config->mmio_regmap;
	struct adi_system_context *ctx;
	size_t i;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->regmap = regmap;
	INIT_RADIX_TREE(&ctx->tree, GFP_KERNEL);

	for (i = 0; i < config->len; ++i) {
		struct adi_system_register *sreg = &config->registers[i];

		ret = radix_tree_insert(&ctx->tree, sreg->id, sreg);
		if (ret)
			return ERR_PTR(ret);
	}

	config->config.max_register = config->max_register;
	config->config.reg_bits = 8 * sizeof(uint32_t);
	config->config.val_bits = 8 * sizeof(uint32_t);
	config->config.reg_stride = 1;

	return ctx;
}

static void regmap_system_free_context(void *context)
{
	struct adi_system_context *ctx = context;
	unsigned int i;

	for (i = 0; i < ctx->config->len; ++i)
		radix_tree_delete(&ctx->tree,
				ctx->config->registers[i].id);

	kfree(ctx);
}

static const struct regmap_bus regmap_system_bus = {
	.fast_io = true,
	.reg_write = regmap_system_write,
	.reg_read = regmap_system_read,
	.free_context = regmap_system_free_context,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

static struct regmap *__devm_regmap_init_adi_system_config(struct device *dev,
	struct adi_system_config *config,
	struct lock_class_key *lock_key, const char *lock_name)
{
	struct adi_system_context *ctx = create_context(config);

	if (IS_ERR(ctx))
		return ERR_PTR(PTR_ERR(ctx));

	return __devm_regmap_init(dev, &regmap_system_bus, ctx,
			&config->config,
		lock_key, lock_name);
}

static DEFINE_SPINLOCK(adi_system_config_lock);
static LIST_HEAD(adi_system_config_list);

static int adi_system_config_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct adi_system_config *config = &adi_pads_config;
	struct device_node *np = dev->of_node;
	struct regmap *regmap_mmio;
	struct regmap *regmap_system;
	struct resource *res;
	void __iomem *base;
	unsigned long flags;

	struct regmap_config mmio_config = {
		.reg_bits = 8 * sizeof(uint32_t),
		.val_bits = 8 * sizeof(uint32_t),
		.reg_stride = sizeof(uint32_t),
	};

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	base = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(base))
		return PTR_ERR(base);

	mmio_config.name = dev_name(dev);
	mmio_config.max_register = resource_size(res) - sizeof(uint32_t);

	regmap_mmio = devm_regmap_init_mmio(dev, base, &mmio_config);
	if (IS_ERR(regmap_mmio)) {
		dev_err(dev, "mmio regmap initialization failed\n");
		return PTR_ERR(regmap_mmio);
	}

	config->mmio_regmap = regmap_mmio;
	regmap_system = devm_regmap_init_adi_system_config(dev, config);
	if (IS_ERR(regmap_system)) {
		dev_err(dev, "system config regmap initialization failed\n");
		return PTR_ERR(regmap_system);
	}

	config->np = np;
	config->system_regmap = regmap_system;
	platform_set_drvdata(pdev, config);

	spin_lock_irqsave(&adi_system_config_lock, flags);
	list_add_tail(&config->list, &adi_system_config_list);
	spin_unlock_irqrestore(&adi_system_config_lock, flags);
	return 0;
}

static void adi_system_config_remove(struct platform_device *pdev)
{
	struct adi_system_config *config = platform_get_drvdata(pdev);
	unsigned long flags;

	spin_lock_irqsave(&adi_system_config_lock, flags);
	list_del(&config->list);
	spin_unlock_irqrestore(&adi_system_config_lock, flags);
}

/*
 * PADs configuration registers are required to configure peripherals,
 * and by extension the system. Hence the driver focuses on driving them while
 * also setting up the remaining system.
 */
static const struct of_device_id pads_dt_ids[] = {
	{ .compatible = "adi,pads-peripheral-config", },
	{ }
};
MODULE_DEVICE_TABLE(of, pads_dt_ids);

static struct platform_driver pads_driver = {
	.driver = {
		.name = "adi-system-config",
		.of_match_table = pads_dt_ids,
	},
	.probe = adi_system_config_probe,
	.remove = adi_system_config_remove,
};
module_platform_driver(pads_driver);

MODULE_AUTHOR("Greg Malysa <greg.malysa@timesys.com>");
MODULE_DESCRIPTION("ADI ADSP PADS CFG-based System Configuration Driver");
MODULE_LICENSE("GPL v2");