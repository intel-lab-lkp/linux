// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Intel Xe SMBus handling
 *
 * Copyright (C) 2026 Intel Corporation.
 */

#include <linux/i2c-smbus.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include "xe_i2c.h"

/**
 * DOC: Handler for SMBus Alerts
 *
 * If the I2C controller on the platform supports the SMBus extensions, a
 * dedicated SMBus alert device needs to be registered for it. The alert device
 * handles the standard SMBus alert processing and forwarding of the alert to
 * the correct I2C client driver. On most platforms the i2c client device that
 * generates the alerts will be the AMC.
 */

#define DW_IC_SMBUS_INTR_STAT			0xc8
#define DW_IC_CLR_SMBUS_INTR			0xd4

#define DW_IC_SMBUS_INTR_ALERT			BIT(10)

void xe_i2c_handle_smbus_alert(struct xe_i2c *i2c)
{
	u32 stat;

	if (!i2c->smbus_alert)
		return;

	regmap_read(i2c->regmap, DW_IC_SMBUS_INTR_STAT, &stat);
	if (!stat)
		return;

	regmap_write(i2c->regmap, DW_IC_CLR_SMBUS_INTR, stat);

	if (stat & DW_IC_SMBUS_INTR_ALERT)
		i2c_handle_smbus_alert(i2c->smbus_alert);
}

static struct i2c_smbus_alert_setup xe_i2c_smbus_setup;

int xe_i2c_register_smbus_alert(struct xe_i2c *i2c)
{
	struct i2c_client *alert;

	alert = i2c_new_smbus_alert_device(i2c->adapter, &xe_i2c_smbus_setup);
	if (IS_ERR(alert))
		return PTR_ERR(alert);

	i2c->smbus_alert = alert;

	return 0;
}
