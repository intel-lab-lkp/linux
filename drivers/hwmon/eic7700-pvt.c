// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN EIC7700 Process, Voltage, Temperature sensor driver
 *
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors:
 *   Yulin Lu <luyulin@eswincomputing.com>
 *   Huan He <hehuan1@eswincomputing.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon-sysfs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/polynomial.h>
#include <linux/reset.h>
#include "eic7700-pvt.h"

/*
 * For the sake of the code simplification we created the sensors info table
 * with the sensor names, activation modes.
 */
static int eic7700_pvt_create_sensor_info(struct pvt_hwmon *pvt)
{
	const char *suffixes[PVT_SENSORS_NUM] = {
		" Temperature",
		" Voltage",
		" Low-Vt",
		" UltraLow-Vt",
		" Standard-Vt"
	};
	struct device *dev = pvt->dev;
	struct device_node *np = dev->of_node;
	struct pvt_sensor_info *info_array;
	char *labels[PVT_SENSORS_NUM];
	const char *prefix = NULL;
	const char *node_label;
	int i;

	if (of_property_read_string(np, "label", &node_label)) {
		dev_err(dev, "Missing 'label' property in DTS node\n");
		return -EINVAL;
	}

	struct {
		const char *label;
		const char *prefix;
	} label_map[] = {
		{ "pvt0", "SoC"},
		{ "pvt1", "DDR Core"},
	};

	for (i = 0; i < ARRAY_SIZE(label_map); i++) {
		if (strcmp(node_label, label_map[i].label) == 0) {
			prefix = label_map[i].prefix;
			break;
		}
	}

	info_array = devm_kzalloc(dev, PVT_SENSORS_NUM * sizeof(*info_array),
				  GFP_KERNEL);
	if (!info_array)
		return -ENOMEM;

	for (i = 0; i < PVT_SENSORS_NUM; i++) {
		labels[i] = devm_kasprintf(dev, GFP_KERNEL, "%s%s", prefix,
					   suffixes[i]);
		if (!labels[i])
			return -ENOMEM;
	}

	info_array[0] = (struct pvt_sensor_info)PVT_SENSOR_INFO(0, labels[0],
								hwmon_temp,
								TEMP);
	info_array[1] = (struct pvt_sensor_info)PVT_SENSOR_INFO(0, labels[1],
								hwmon_in,
								VOLT);
	info_array[2] = (struct pvt_sensor_info)PVT_SENSOR_INFO(1, labels[2],
								hwmon_in,
								LVT);
	info_array[3] = (struct pvt_sensor_info)PVT_SENSOR_INFO(2, labels[3],
								hwmon_in,
								ULVT);
	info_array[4] = (struct pvt_sensor_info)PVT_SENSOR_INFO(3, labels[4],
								hwmon_in,
								SVT);

	pvt->sensor_info = info_array;
	return 0;
}

/*
 * The original translation formulae of the temperature (in degrees of Celsius)
 * to PVT data and vice-versa are following:
 * N = 6.0818e-8*(T^4) +1.2873e-5*(T^3) + 7.2244e-3*(T^2) + 3.6484*(T^1) +
 *     1.6198e2,
 * T = -1.8439e-11*(N^4) + 8.0705e-8*(N^3) + -1.8501e-4*(N^2) +
 *     3.2843e-1*(N^1) - 4.8690e1,
 * where T = [-40, 125]C and N = [27, 771].
 * They must be accordingly altered to be suitable for the integer arithmetics.
 * The technique is called 'factor redistribution', which just makes sure the
 * multiplications and divisions are made so to have a result of the operations
 * within the integer numbers limit. In addition we need to translate the
 * formulae to accept millidegrees of Celsius. Here what they look like after
 * the alterations:
 * N = (60818e-20*(T^4) + 12873e-14*(T^3) + 72244e-9*(T^2) + 36484e-3*T +
 *     16198e2) / 1e4,
 * T = -18439e-12*(N^4) + 80705e-9*(N^3) - 185010e-6*(N^2) + 328430e-3*N -
 *     48690,
 * where T = [-40000, 125000] mC and N = [27, 771].
 */
static const struct polynomial poly_N_to_temp = {
	.total_divider = 1,
	.terms = {
		{4, -18439, 1000, 1},
		{3, 80705, 1000, 1},
		{2, -185010, 1000, 1},
		{1, 328430, 1000, 1},
		{0, -48690, 1, 1}
	}
};

/*
 * Similar alterations are performed for the voltage conversion equations.
 * The original formulae are:
 * N = 1.3905e3*V - 5.7685e2,
 * V = (N + 5.7685e2) / 1.3905e3,
 * where V = [0.72, 0.88] V and N = [424, 646].
 * After the optimization they looks as follows:
 * N = (13905e-3*V - 5768.5) / 10,
 * V = (N * 10^5 / 13905 + 57685 * 10^3 / 13905) / 10.
 * where V = [720, 880] mV and N = [424, 646].
 */
static const struct polynomial poly_N_to_volt = {
	.total_divider = 10,
	.terms = {
		{1, 100000, 13905, 1},
		{0, 57685000, 1, 13905}
	}
};

static inline u32 eic7700_pvt_update(void __iomem *reg, u32 mask, u32 data)
{
	u32 old;

	old = readl_relaxed(reg);
	writel((old & ~mask) | (data & mask), reg);

	return old & mask;
}

static inline void eic7700_pvt_set_mode(struct pvt_hwmon *pvt, u32 mode)
{
	u32 old;

	mode = FIELD_PREP(PVT_MODE_MASK, mode);

	old = eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, 0);
	eic7700_pvt_update(pvt->regs + PVT_MODE, PVT_MODE_MASK, mode);
	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, old);
}

static inline void eic7700_pvt_set_trim(struct pvt_hwmon *pvt, u32 val)
{
	u32 old;

	old = eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, 0);
	writel(val, pvt->regs + PVT_TRIM);
	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, old);
}

static irqreturn_t eic7700_pvt_hard_isr(int irq, void *data)
{
	struct pvt_hwmon *pvt = data;
	struct pvt_cache *cache;
	u32 val;

	eic7700_pvt_update(pvt->regs + PVT_INT, PVT_INT_CLR, PVT_INT_CLR);

	/*
	 * Read the data, update the cache and notify a waiter of this event.
	 */

	val = readl(pvt->regs + PVT_DATA);

	cache = &pvt->cache[pvt->sensor];

	WRITE_ONCE(cache->data, FIELD_GET(PVT_DATA_OUT, val));

	complete(&cache->conversion);

	return IRQ_HANDLED;
}

static int eic7700_pvt_read_data(struct pvt_hwmon *pvt,
				 enum pvt_sensor_type type, long *val)
{
	const struct pvt_sensor_info *pvt_info = pvt->sensor_info;
	struct pvt_cache *cache = &pvt->cache[type];
	u32 data;
	int ret;

	if (!pvt_info) {
		dev_err(pvt->dev, "No matching device data found\n");
		return -EINVAL;
	}
	/*
	 * Lock PVT conversion interface until data cache is updated. The data
	 * read procedure is following: set the requested PVT sensor mode,
	 * enable conversion, wait until conversion is finished, then disable
	 * conversion and IRQ, and read the cached data.
	 */
	ret = mutex_lock_interruptible(&pvt->iface_mtx);
	if (ret)
		return ret;

	pvt->sensor = type;
	eic7700_pvt_set_mode(pvt, pvt_info[type].mode);

	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, PVT_ENA_EN);

	ret = wait_for_completion_interruptible(&cache->conversion);

	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, 0);
	eic7700_pvt_update(pvt->regs + PVT_INT, PVT_INT_CLR, PVT_INT_CLR);

	data = READ_ONCE(cache->data);

	mutex_unlock(&pvt->iface_mtx);

	if (ret && (ret != -ERESTARTSYS))
		return ret;

	if (type == PVT_TEMP)
		*val = polynomial_calc(&poly_N_to_temp, data);
	else if (type == PVT_VOLT)
		*val = polynomial_calc(&poly_N_to_volt, data);
	else
		*val = data;

	return 0;
}

static const struct hwmon_channel_info *pvt_channel_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_TYPE | HWMON_T_LABEL |
			   HWMON_T_OFFSET),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	NULL
};

static inline bool
eic7700_pvt_hwmon_channel_is_valid(enum hwmon_sensor_types type, int ch)
{
	switch (type) {
	case hwmon_temp:
		if (ch < 0 || ch >= PVT_TEMP_CHS)
			return false;
		break;
	case hwmon_in:
		if (ch < 0 || ch >= PVT_VOLT_CHS)
			return false;
		break;
	default:
		break;
	}

	return true;
}

static umode_t eic7700_pvt_hwmon_is_visible(const void *data,
					    enum hwmon_sensor_types type,
					    u32 attr, int ch)
{
	if (!eic7700_pvt_hwmon_channel_is_valid(type, ch))
		return 0;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_type:
		case hwmon_temp_label:
			return 0444;
		case hwmon_temp_offset:
			return 0644;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			return 0444;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int eic7700_pvt_read_trim(struct pvt_hwmon *pvt, long *val)
{
	u32 data;

	data = readl(pvt->regs + PVT_TRIM);
	*val = data;

	return 0;
}

static int eic7700_pvt_write_trim(struct pvt_hwmon *pvt, long val)
{
	int ret;
	/*
	 * Serialize trim update, since a part of the register is changed and
	 * the controller is supposed to be disabled during this operation.
	 */
	ret = mutex_lock_interruptible(&pvt->iface_mtx);
	if (ret)
		return ret;

	eic7700_pvt_set_trim(pvt, val);

	mutex_unlock(&pvt->iface_mtx);

	return 0;
}

static int eic7700_pvt_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int ch, long *val)
{
	struct pvt_hwmon *pvt = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_get_sync(pvt->dev);
	if (ret < 0) {
		dev_err(pvt->dev, "Failed to resume PVT device: %d\n", ret);
		return ret;
	}

	if (!eic7700_pvt_hwmon_channel_is_valid(type, ch)) {
		ret = -EINVAL;
		goto out_put;
	}

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			ret = eic7700_pvt_read_data(pvt, ch, val);
			break;
		case hwmon_temp_type:
			*val = 1;
			ret = 0;
			break;
		case hwmon_temp_offset:
			ret = eic7700_pvt_read_trim(pvt, val);
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_in:
		if (attr == hwmon_in_input)
			ret = eic7700_pvt_read_data(pvt, PVT_VOLT + ch, val);
		else
			ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EOPNOTSUPP;
	}

out_put:
	pm_runtime_mark_last_busy(pvt->dev);
	pm_runtime_put_autosuspend(pvt->dev);
	return ret;
}

static int eic7700_pvt_hwmon_read_string(struct device *dev,
					 enum hwmon_sensor_types type, u32 attr,
					 int ch, const char **str)
{
	struct pvt_hwmon *pvt = dev_get_drvdata(dev);
	const struct pvt_sensor_info *pvt_info = pvt->sensor_info;

	if (!eic7700_pvt_hwmon_channel_is_valid(type, ch))
		return -EINVAL;

	if (!pvt_info) {
		dev_err(pvt->dev, "No matching device data found\n");
		return -EINVAL;
	}

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_label) {
			*str = pvt_info[ch].label;
			return 0;
		}
		break;
	case hwmon_in:
		if (attr == hwmon_in_label) {
			*str = pvt_info[PVT_VOLT + ch].label;
			return 0;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int eic7700_pvt_hwmon_write(struct device *dev,
				   enum hwmon_sensor_types type, u32 attr,
				   int ch, long val)
{
	struct pvt_hwmon *pvt = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_get_sync(pvt->dev);
	if (ret < 0) {
		dev_err(pvt->dev, "Failed to resume PVT device: %d\n", ret);
		return ret;
	}

	if (!eic7700_pvt_hwmon_channel_is_valid(type, ch)) {
		ret = -EINVAL;
		goto out_put;
	}

	if (type == hwmon_temp || attr == hwmon_temp_offset)
		ret = eic7700_pvt_write_trim(pvt, val);
	else
		ret = -EOPNOTSUPP;

out_put:
	pm_runtime_mark_last_busy(pvt->dev);
	pm_runtime_put_autosuspend(pvt->dev);
	return ret;
}

static const struct hwmon_ops pvt_hwmon_ops = {
	.is_visible = eic7700_pvt_hwmon_is_visible,
	.read = eic7700_pvt_hwmon_read,
	.read_string = eic7700_pvt_hwmon_read_string,
	.write = eic7700_pvt_hwmon_write
};

static const struct hwmon_chip_info pvt_hwmon_info = {
	.ops = &pvt_hwmon_ops,
	.info = pvt_channel_info
};

static void pvt_clear_data(void *data)
{
	struct pvt_hwmon *pvt = data;
	int idx;

	for (idx = 0; idx < PVT_SENSORS_NUM; ++idx)
		complete_all(&pvt->cache[idx].conversion);

	mutex_destroy(&pvt->iface_mtx);
}

static struct pvt_hwmon *eic7700_pvt_create_data(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pvt_hwmon *pvt;
	int ret, idx;

	pvt = devm_kzalloc(dev, sizeof(*pvt), GFP_KERNEL);
	if (!pvt)
		return ERR_PTR(-ENOMEM);

	ret = devm_add_action(dev, pvt_clear_data, pvt);
	if (ret) {
		dev_err(dev, "Can't add PVT data clear action\n");
		return ERR_PTR(ret);
	}

	pvt->dev = dev;
	pvt->sensor = PVT_SENSOR_FIRST;
	mutex_init(&pvt->iface_mtx);

	for (idx = 0; idx < PVT_SENSORS_NUM; ++idx)
		init_completion(&pvt->cache[idx].conversion);

	return pvt;
}

static void eic7700_pvt_remove(void *data)
{
	int ret;
	struct pvt_hwmon *pvt = data;

	pm_runtime_disable(pvt->dev);
	pm_runtime_dont_use_autosuspend(pvt->dev);
	pm_runtime_get_sync(pvt->dev);

	ret = reset_control_assert(pvt->rst);
	if (ret)
		dev_err(pvt->dev, "Failed to assert reset: %d\n", ret);

	if (pm_runtime_active(pvt->dev))
		clk_disable_unprepare(pvt->clk);

	pm_runtime_put_noidle(pvt->dev);
}

static int eic7700_pvt_check_pwr(struct pvt_hwmon *pvt)
{
	unsigned long tout;
	int ret = 0;

	/*
	 * Test out the sensor conversion functionality. If it is not done on
	 * time then the domain must have been unpowered and we won't be able
	 * to use the device later in this driver.
	 */
	eic7700_pvt_update(pvt->regs + PVT_INT, PVT_INT_CLR, PVT_INT_CLR);
	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, PVT_ENA_EN);
	readl(pvt->regs + PVT_DATA);

	tout = PVT_TOUT_MIN / NSEC_PER_USEC;
	usleep_range(tout, 2 * tout);

	readl(pvt->regs + PVT_DATA);
	if (!(readl(pvt->regs + PVT_INT) & PVT_INT_STAT)) {
		ret = -ENODEV;
		dev_err(pvt->dev,
			"Sensor is powered down - no interrupt generated\n");
	}

	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, 0);
	eic7700_pvt_update(pvt->regs + PVT_INT, PVT_INT_CLR, PVT_INT_CLR);

	return ret;
}

static int eic7700_pvt_init_iface(struct pvt_hwmon *pvt)
{
	unsigned long rate;
	const struct pvt_sensor_info *pvt_info = pvt->sensor_info;

	rate = clk_get_rate(pvt->clk);
	if (!rate) {
		dev_err(pvt->dev, "Invalid reference clock rate\n");
		return -ENODEV;
	}
	if (!pvt_info) {
		dev_err(pvt->dev, "No matching device data found\n");
		return -EINVAL;
	}
	/*
	 * Make sure controller are disabled so not to accidentally have ISR
	 * executed before the driver data is fully initialized. Clear the IRQ
	 * status as well.
	 */
	eic7700_pvt_update(pvt->regs + PVT_INT, PVT_INT_CLR, PVT_INT_CLR);
	eic7700_pvt_update(pvt->regs + PVT_ENA, PVT_ENA_EN, 0);
	readl(pvt->regs + PVT_INT);
	readl(pvt->regs + PVT_DATA);

	/* Setup default sensor mode and temperature trim. */
	eic7700_pvt_set_mode(pvt, pvt_info[pvt->sensor].mode);

	eic7700_pvt_set_trim(pvt, PVT_TRIM_DEF);

	return 0;
}

static int eic7700_pvt_request_irq(struct pvt_hwmon *pvt)
{
	struct platform_device *pdev = to_platform_device(pvt->dev);
	int ret;

	pvt->irq = platform_get_irq(pdev, 0);
	if (pvt->irq < 0)
		return pvt->irq;

	ret = devm_request_threaded_irq(pvt->dev, pvt->irq,
					eic7700_pvt_hard_isr, NULL,
					IRQF_SHARED | IRQF_TRIGGER_HIGH, "pvt",
					pvt);
	if (ret) {
		dev_err(pvt->dev, "Couldn't request PVT IRQ\n");
		return ret;
	}

	return 0;
}

static int eic7700_pvt_create_hwmon(struct pvt_hwmon *pvt)
{
	struct device *dev = pvt->dev;
	struct device_node *np = dev->of_node;
	const char *node_label;
	int type;
	const char *names[2] = {"soc_pvt", "ddr_pvt"};

	if (of_property_read_string(np, "label", &node_label)) {
		dev_err(dev, "Missing 'label' property in DTS node\n");
		return -EINVAL;
	}

	if (strcmp(node_label, "pvt0") == 0) {
		type = 0;
	} else if (strcmp(node_label, "pvt1") == 0) {
		type = 1;
	} else {
		dev_err(pvt->dev, "Unsupported label: %s\n", node_label);
		return -EINVAL;
	}

	pvt->hwmon = devm_hwmon_device_register_with_info(pvt->dev, names[type],
							  pvt, &pvt_hwmon_info,
							  NULL);
	if (IS_ERR(pvt->hwmon)) {
		dev_err(pvt->dev, "Couldn't create hwmon device\n");
		return PTR_ERR(pvt->hwmon);
	}

	return 0;
}

static int eic7700_pvt_probe(struct platform_device *pdev)
{
	struct pvt_hwmon *pvt;
	int ret;

	pvt = eic7700_pvt_create_data(pdev);
	if (IS_ERR(pvt))
		return PTR_ERR(pvt);

	platform_set_drvdata(pdev, pvt);

	ret = eic7700_pvt_create_sensor_info(pvt);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create sensor info\n");
		return ret;
	}

	pvt->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pvt->regs))
		return PTR_ERR(pvt->regs);

	pvt->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(pvt->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pvt->clk),
				     "Couldn't get clock\n");

	pvt->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(pvt->rst))
		return dev_err_probe(pvt->dev, PTR_ERR(pvt->rst),
				     "Couldn't to get reset control\n");

	if (pvt->rst) {
		ret = reset_control_reset(pvt->rst);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Couldn't to trigger reset\n");
	}

	ret = eic7700_pvt_check_pwr(pvt);
	if (ret)
		return ret;

	ret = eic7700_pvt_init_iface(pvt);
	if (ret)
		return ret;

	ret = eic7700_pvt_request_irq(pvt);
	if (ret)
		return ret;

	pm_runtime_enable(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, 3000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	ret = eic7700_pvt_create_hwmon(pvt);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(pvt->dev, eic7700_pvt_remove, pvt);
	if (ret) {
		dev_err(pvt->dev, "Can't add PVT clocks disable action\n");
		return ret;
	}

	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;
}

static int __maybe_unused eic7700_pvt_runtime_resume(struct device *dev)
{
	struct pvt_hwmon *pvt = dev_get_drvdata(dev);
	int ret;

	if (!pvt->clk) {
		dev_err(dev, "clk not initialized\n");
		return -ENODEV;
	}

	ret = clk_prepare_enable(pvt->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock: %d\n", ret);
		return ret;
	}

	return 0;
}

static int __maybe_unused eic7700_pvt_runtime_suspend(struct device *dev)
{
	struct pvt_hwmon *pvt = dev_get_drvdata(dev);

	if (pvt->clk)
		clk_disable_unprepare(pvt->clk);

	return 0;
}

static int __maybe_unused eic7700_pvt_suspend(struct device *dev)
{
	int ret = 0;

	if (!pm_runtime_status_suspended(dev)) {
		ret = eic7700_pvt_runtime_suspend(dev);
		if (ret) {
			dev_err(dev, "Failed to suspend: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int __maybe_unused eic7700_pvt_resume(struct device *dev)
{
	int ret = 0;

	if (!pm_runtime_status_suspended(dev)) {
		ret = eic7700_pvt_runtime_resume(dev);
		if (ret) {
			dev_err(dev, "Failed to resume: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static const struct dev_pm_ops eic7700_pvt_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(eic7700_pvt_suspend, eic7700_pvt_resume)
	RUNTIME_PM_OPS(eic7700_pvt_runtime_suspend, eic7700_pvt_runtime_resume,
		       NULL)
};

static const struct of_device_id pvt_of_match[] = {
	{ .compatible = "eswin,eic7700-pvt"},
	{ }
};
MODULE_DEVICE_TABLE(of, pvt_of_match);

static struct platform_driver pvt_driver = {
	.probe = eic7700_pvt_probe,
	.driver = {
		.name = "eic7700-pvt",
		.of_match_table = pvt_of_match,
		.pm = pm_sleep_ptr(&eic7700_pvt_pm_ops),
	},
};
module_platform_driver(pvt_driver);

MODULE_AUTHOR("Yulin Lu <luyulin@eswincomputing.com>");
MODULE_AUTHOR("Huan He <hehuan1@eswincomputing.com>");
MODULE_DESCRIPTION("Eswin eic7700 PVT driver");
MODULE_LICENSE("GPL");
