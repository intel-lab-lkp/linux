// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Bharadwaj Raju <bharadwaj.raju777@gmail.com>
 */

#include "inv_icm20948.h"

static int inv_icm20948_suspend(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	struct inv_icm20948_state *state = dev_get_drvdata(dev);

	guard(mutex)(&state->lock);

	return regmap_write_bits(state->regmap, INV_ICM20948_REG_PWR_MGMT_1,
				 INV_ICM20948_PWR_MGMT_1_SLEEP,
				 INV_ICM20948_PWR_MGMT_1_SLEEP);
}

static int inv_icm20948_resume(struct device *dev)
{
	struct inv_icm20948_state *state = dev_get_drvdata(dev);

	guard(mutex)(&state->lock);

	pm_runtime_disable(state->dev);
	pm_runtime_set_active(state->dev);
	pm_runtime_enable(state->dev);

	int ret = regmap_write_bits(state->regmap, INV_ICM20948_REG_PWR_MGMT_1,
				    INV_ICM20948_PWR_MGMT_1_SLEEP, 0);
	if (ret)
		return ret;

	msleep(INV_ICM20948_SLEEP_WAKEUP_MS);

	return 0;
}

static void inv_icm20948_pm_disable(void *data)
{
	struct device *dev = data;

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

int inv_icm20948_pm_setup(struct inv_icm20948_state *state)
{
	struct device *dev = state->dev;

	guard(mutex)(&state->lock);

	int ret;

	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, INV_ICM20948_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return devm_add_action_or_reset(dev, inv_icm20948_pm_disable, dev);
}

EXPORT_NS_GPL_DEV_PM_OPS(inv_icm20948_pm_ops, IIO_ICM20948) = {
	SYSTEM_SLEEP_PM_OPS(inv_icm20948_suspend, inv_icm20948_resume)
	RUNTIME_PM_OPS(inv_icm20948_suspend, inv_icm20948_resume, NULL)
};
