/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _I2C_ASPEED_CORE_H
#define _I2C_ASPEED_CORE_H

#include <linux/of.h>
#include <linux/platform_device.h>

enum i2c_version {
	AST2400_I2C,
	AST2500_I2C,
	AST2600_I2C,
	AST2700_I2C,
};

int aspeed_i2c_probe_bus(const struct of_device_id *match, struct platform_device *pdev);
void aspeed_i2c_remove_bus(struct platform_device *pdev);
int ast2600_i2c_probe(const struct of_device_id *match, struct platform_device *pdev);
void ast2600_i2c_remove(struct platform_device *pdev);
#endif
