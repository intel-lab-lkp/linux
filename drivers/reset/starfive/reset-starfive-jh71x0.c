// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reset driver for the StarFive JH71X0 SoCs
 *
 * Copyright (C) 2021 Emil Renner Berthing <kernel@esmil.dk>
 */

#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#include "reset-starfive-jh71x0.h"

#define JH71X0_RESET_ID_INVALID	ULONG_MAX

struct jh71x0_reset {
	struct reset_controller_dev rcdev;
	/* protect registers against concurrent read-modify-write */
	spinlock_t lock;
	void __iomem *assert;
	void __iomem *status;
	const u32 *asserted;

	/* Only exists in reset controllers that use the
	 * reset_starfive_jh71x0_register_with_info helper.
	 */
	const struct jh71x0_reset_info *info;
};

static inline struct jh71x0_reset *
jh71x0_reset_from(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct jh71x0_reset, rcdev);
}

static unsigned long
jh71x0_reset_id_to_hw_id(const struct jh71x0_reset_map *map, unsigned int nr_resets,
			 unsigned long reset_id)
{
	for (u32 i = 0; i < nr_resets; i++) {
		if (map[i].reset_id == reset_id)
			return map[i].hw_id;
	}

	return JH71X0_RESET_ID_INVALID;
}

static int jh71x0_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	struct jh71x0_reset *data = jh71x0_reset_from(rcdev);
	unsigned long offset, flags;
	void __iomem *reg_assert;
	void __iomem *reg_status;
	u32 mask, done, value;
	int ret;

	if (data->info && data->info->discontiguous) {
		id = jh71x0_reset_id_to_hw_id(data->info->map, data->info->nr_resets, id);
		if (id == JH71X0_RESET_ID_INVALID)
			return -EINVAL;
	}

	offset = id / 32;
	mask = BIT(id % 32);
	reg_assert = data->assert + offset * sizeof(u32);
	reg_status = data->status + offset * sizeof(u32);
	done = data->asserted ? data->asserted[offset] & mask : 0;

	if (!assert)
		done ^= mask;

	spin_lock_irqsave(&data->lock, flags);

	value = readl(reg_assert);
	if (assert)
		value |= mask;
	else
		value &= ~mask;
	writel(value, reg_assert);

	/* if the associated clock is gated, deasserting might otherwise hang forever */
	ret = readl_poll_timeout_atomic(reg_status, value, (value & mask) == done, 0, 1000);

	spin_unlock_irqrestore(&data->lock, flags);
	return ret;
}

static int jh71x0_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return jh71x0_reset_update(rcdev, id, true);
}

static int jh71x0_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return jh71x0_reset_update(rcdev, id, false);
}

static int jh71x0_reset_reset(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	int ret;

	ret = jh71x0_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return jh71x0_reset_deassert(rcdev, id);
}

static int jh71x0_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct jh71x0_reset *data = jh71x0_reset_from(rcdev);
	void __iomem *reg_status;
	unsigned long offset;
	u32 mask, value;

	if (data->info && data->info->discontiguous) {
		id = jh71x0_reset_id_to_hw_id(data->info->map, data->info->nr_resets, id);
		if (id == JH71X0_RESET_ID_INVALID)
			return -EINVAL;
	}

	offset = id / 32;
	mask = BIT(id % 32);
	reg_status = data->status + offset * sizeof(u32);
	value = readl(reg_status);

	if (!data->asserted)
		return !(value & mask);

	return !((value ^ data->asserted[offset]) & mask);
}

static const struct reset_control_ops jh71x0_reset_ops = {
	.assert		= jh71x0_reset_assert,
	.deassert	= jh71x0_reset_deassert,
	.reset		= jh71x0_reset_reset,
	.status		= jh71x0_reset_status,
};

int reset_starfive_jh71x0_register(struct device *dev, struct device_node *of_node,
				   void __iomem *assert, void __iomem *status,
				   const u32 *asserted, unsigned int nr_resets,
				   struct module *owner)
{
	struct jh71x0_reset *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->rcdev.ops = &jh71x0_reset_ops;
	data->rcdev.owner = owner;
	data->rcdev.nr_resets = nr_resets;
	data->rcdev.dev = dev;
	data->rcdev.of_node = of_node;

	spin_lock_init(&data->lock);
	data->assert = assert;
	data->status = status;
	data->asserted = asserted;

	return devm_reset_controller_register(dev, &data->rcdev);
}
EXPORT_SYMBOL_GPL(reset_starfive_jh71x0_register);

int reset_starfive_jh71x0_register_with_info(struct device *dev, struct device_node *of_node,
					     void __iomem *assert, void __iomem *status,
					     const u32 *asserted,
					     const struct jh71x0_reset_info *info,
					     struct module *owner)
{
	struct jh71x0_reset *data;
	int ret;

	if (info->discontiguous && (!info->map || !info->nr_resets))
		return dev_err_probe(dev, -EINVAL,
				     "discontiguous controller without a valid ID map\n");

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->rcdev.ops = &jh71x0_reset_ops;
	data->rcdev.owner = owner;
	data->rcdev.nr_resets = info->nr_resets;
	data->rcdev.dev = dev;
	data->rcdev.of_node = of_node;

	spin_lock_init(&data->lock);
	data->assert = assert;
	data->status = status;
	data->asserted = asserted;
	data->info = info;

	ret = devm_reset_controller_register(dev, &data->rcdev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register reset controller\n");

	dev_info(dev, "Registered %u resets\n", data->rcdev.nr_resets);

	return 0;
}
EXPORT_SYMBOL_GPL(reset_starfive_jh71x0_register_with_info);
