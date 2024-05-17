// SPDX-License-Identifier: GPL-2.0-only
//
// Register map access API - A2B support
//
// Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>

#include <linux/regmap.h>
#include <linux/a2b/a2b.h>

static int regmap_a2b_write(void *context, const void *data, size_t count)
{
	struct a2b_node *node = context;
	struct a2b_bus *bus = node->bus;
	const u8 *d = data;
	u8 reg;
	int ret;
	int i;

	reg = d[0];

	for (i = 0; i < count - 1; i++) {
		ret = bus->ops->write(bus, node, reg + i, d[i + 1]);
		if (ret)
			return ret;
	}

	return 0;
}

static int regmap_a2b_read(void *context, const void *reg_buf, size_t reg_size,
			   void *val_buf, size_t val_size)
{
	struct a2b_node *node = context;
	struct a2b_bus *bus = node->bus;
	u8 reg = ((u8 *)reg_buf)[0];
	u8 *v = val_buf;
	int ret;
	int i;

	if (reg_size != 1)
		return -EINVAL;

	for (i = 0; i < val_size; i++) {
		unsigned int tmp;

		ret = bus->ops->read(bus, node, reg + i, &tmp);
		if (ret)
			return ret;

		v[i] = tmp & 0xFF;
	}

	return 0;
}

static const struct regmap_bus regmap_a2b = {
	.write = regmap_a2b_write,
	.read = regmap_a2b_read,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

struct regmap *__devm_regmap_init_a2b_node(struct a2b_node *node,
					   const struct regmap_config *config,
					   struct lock_class_key *lock_key,
					   const char *lock_name)
{
	return __devm_regmap_init(&node->dev, &regmap_a2b, node, config,
				  lock_key, lock_name);
}
EXPORT_SYMBOL_GPL(__devm_regmap_init_a2b_node);

struct regmap *__devm_regmap_init_a2b_func(struct a2b_func *func,
					   const struct regmap_config *config,
					   struct lock_class_key *lock_key,
					   const char *lock_name)
{
	return __devm_regmap_init(&func->dev, &regmap_a2b, func->node, config,
				  lock_key, lock_name);
}
EXPORT_SYMBOL_GPL(__devm_regmap_init_a2b_func);

MODULE_LICENSE("GPL");
