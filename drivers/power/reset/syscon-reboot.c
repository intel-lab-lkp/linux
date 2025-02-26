// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic Syscon Reboot Driver
 *
 * Copyright (c) 2013, Applied Micro Circuits Corporation
 * Author: Feng Kan <fkan@apm.com>
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

/* REBOOT_GPIO doesn't make sense for syscon-reboot */
static const struct {
	enum reboot_mode mode;
	const char *prefix;
} prefix_map[] = {
	{ .mode = REBOOT_COLD, .prefix = "cold"  },
	{ .mode = REBOOT_WARM, .prefix = "warm"  },
	{ .mode = REBOOT_HARD, .prefix = "hard"  },
	{ .mode = REBOOT_SOFT, .prefix = "soft"  },
};

struct reboot_mode_bits {
	u32 offset;
	u32 value;
	u32 mask;
	bool valid;
};

struct syscon_reboot_context {
	struct regmap *map;

	struct reboot_mode_bits mode_bits[REBOOT_SOFT + 1];
	struct reboot_mode_bits catchall;
	struct notifier_block restart_handler;
};

static int syscon_restart_handle(struct notifier_block *this,
					unsigned long mode, void *cmd)
{
	struct syscon_reboot_context *ctx =
			container_of(this, struct syscon_reboot_context,
					restart_handler);
	const struct reboot_mode_bits *mode_bits;

	if (mode < ARRAY_SIZE(ctx->mode_bits) && ctx->mode_bits[mode].valid)
		mode_bits = &ctx->mode_bits[mode];
	else
		mode_bits = &ctx->catchall;

	/* Issue the reboot */
	regmap_update_bits(ctx->map, mode_bits->offset, mode_bits->mask,
			   mode_bits->value);

	mdelay(1000);

	pr_emerg("Unable to restart system\n");
	return NOTIFY_DONE;
}

static int syscon_reboot_probe(struct platform_device *pdev)
{
	struct syscon_reboot_context *ctx;
	struct device *dev = &pdev->dev;
	int mask_err, value_err;
	int priority;
	int err;
	char prop[32];

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->map = syscon_regmap_lookup_by_phandle(dev->of_node, "regmap");
	if (IS_ERR(ctx->map)) {
		ctx->map = syscon_node_to_regmap(dev->parent->of_node);
		if (IS_ERR(ctx->map))
			return PTR_ERR(ctx->map);
	}

	if (of_property_read_s32(pdev->dev.of_node, "priority", &priority))
		priority = 192;

	BUILD_BUG_ON(ARRAY_SIZE(prefix_map) != ARRAY_SIZE(ctx->mode_bits));
	BUILD_BUG_ON(ARRAY_SIZE(ctx->mode_bits) <= REBOOT_COLD);
	BUILD_BUG_ON(ARRAY_SIZE(ctx->mode_bits) <= REBOOT_WARM);
	BUILD_BUG_ON(ARRAY_SIZE(ctx->mode_bits) <= REBOOT_HARD);
	BUILD_BUG_ON(ARRAY_SIZE(ctx->mode_bits) <= REBOOT_SOFT);

	for (int i = 0; i < ARRAY_SIZE(prefix_map); ++i) {
		const char * const prefix = prefix_map[i].prefix;
		struct reboot_mode_bits * const mode_bits =
			&ctx->mode_bits[prefix_map[i].mode];

		snprintf(prop, sizeof(prop), "%s-offset", prefix);
		if (of_property_read_u32(pdev->dev.of_node, "offset",
					 &mode_bits->offset))
			continue;

		snprintf(prop, sizeof(prop), "%s-value", prefix);
		if (of_property_read_u32(pdev->dev.of_node, prop,
					 &mode_bits->value)) {
			/* don't support old binding here */
			dev_err(dev, "'%s-value' is mandatory\n", prefix);
			continue;
		}

		snprintf(prop, sizeof(prop), "%s-mask", prefix);
		mask_err = of_property_read_u32(pdev->dev.of_node, prop,
						&mode_bits->mask);

		if (mask_err)
			/* support value without mask*/
			mode_bits->mask = 0xffffffff;

		mode_bits->valid = true;
	}

	/* catch-all entry */
	if (of_property_read_u32(pdev->dev.of_node, "offset",
				 &ctx->catchall.offset))
		if (of_property_read_u32(pdev->dev.of_node, "reg",
					 &ctx->catchall.offset))
			return -EINVAL;

	value_err = of_property_read_u32(pdev->dev.of_node, "value",
					 &ctx->catchall.value);
	mask_err = of_property_read_u32(pdev->dev.of_node, "mask",
					&ctx->catchall.mask);
	if (value_err && mask_err) {
		dev_err(dev, "unable to read 'value' and 'mask'");
		return -EINVAL;
	}

	if (value_err) {
		/* support old binding */
		ctx->catchall.value = ctx->catchall.mask;
		ctx->catchall.mask = 0xFFFFFFFF;
	} else if (mask_err) {
		/* support value without mask */
		ctx->catchall.mask = 0xFFFFFFFF;
	}

	ctx->restart_handler.notifier_call = syscon_restart_handle;
	ctx->restart_handler.priority = priority;
	err = register_restart_handler(&ctx->restart_handler);
	if (err)
		dev_err(dev, "can't register restart notifier (err=%d)\n", err);

	return err;
}

static const struct of_device_id syscon_reboot_of_match[] = {
	{ .compatible = "syscon-reboot" },
	{}
};

static struct platform_driver syscon_reboot_driver = {
	.probe = syscon_reboot_probe,
	.driver = {
		.name = "syscon-reboot",
		.of_match_table = syscon_reboot_of_match,
	},
};
builtin_platform_driver(syscon_reboot_driver);
