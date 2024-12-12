/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2024, Markuss Broks <markuss.broks@gmail.com>
 * Copyright 2024, Maksym Holovach <nergzd@nergzd723.xyz>
 */

#ifndef __EXYNOS_SPEEDY_H
#define __EXYNOS_SPEEDY_H

#include <linux/types.h>

struct device;
struct mutex;
struct platform_device;
struct regmap;

struct speedy_controller {
	struct mutex io_lock;
	struct platform_device *pdev;
	struct regmap *map;
};

struct speedy_device {
	u32 reg;
	struct speedy_controller *speedy;
};

/**
 * exynos_speedy_read() - exynos speedy read operation
 * @device:	pointer to speedy device struct
 * @addr:       address to read
 * @val:        pointer to store result
 *
 * Return: 0 on success, -errno otherwise
 */
int exynos_speedy_read(const struct speedy_device *device, u32 addr, u32 *val);

/**
 * exynos_speedy_write() - exynos speedy write operation
 * @device:	pointer to speedy device struct
 * @addr:       address to write
 * @val:        value to write
 *
 * Return: 0 on success, -errno otherwise
 */
int exynos_speedy_write(const struct speedy_device *device, u32 addr, u32 val);

/**
 * devm_speedy_get_device() - managed get speedy device.
 * @dev:	device pointer requesting speedy device handle.
 *
 * Return: pointer to handle on success, ERR_PTR(-errno) otherwise.
 */
const struct speedy_device *devm_speedy_get_device(struct device *dev);

#endif /* __EXYNOS_SPEEDY_H */
