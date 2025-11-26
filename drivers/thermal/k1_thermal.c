// SPDX-License-Identifier: GPL-2.0
/*
 * Thermal sensor driver for SpacemiT K1 SoC
 *
 * Copyright (C) 2025 Shuwei Wu <shuweiwoo@163.com>
 */
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>

#include "thermal_hwmon.h"

#define MAX_SENSOR_NUMBER		5
#define TEMPERATURE_OFFSET		278

#define K1_TSU_INT_EN			0x14
#define K1_TSU_INT_CLR			0x10
#define K1_TSU_INT_STA			0x18

#define K1_TSU_INT_EN_MASK		BIT(0)
#define K1_TSU_INT_MASK(x)		(GENMASK(2, 1) << ((x) * 2))

#define K1_TSU_EN			0x8
#define K1_TSU_EN_MASK(x)		BIT(x)

#define K1_TSU_DATA_BASE		0x20
#define K1_TSU_DATA(x)			(K1_TSU_DATA_BASE + ((x) / 2) * 4)
#define K1_TSU_DATA_MASK(x)		(((x) % 2) ? GENMASK(31, 16) : GENMASK(15, 0))
#define K1_TSU_DATA_SHIFT(x)		(((x) % 2) ? 16 : 0)

#define K1_TSU_THRSH_BASE		0x40
#define K1_TSU_THRSH(x)			(K1_TSU_THRSH_BASE + ((x) * 4))
#define K1_TSU_THRSH_HIGH_MASK		GENMASK(31, 16)
#define K1_TSU_THRSH_LOW_MASK		GENMASK(15, 0)
#define K1_TSU_THRSH_HIGH_SHIFT		16
#define K1_TSU_THRSH_LOW_SHIFT		0

#define K1_TSU_TIME			0x0C
#define K1_TSU_TIME_MASK		GENMASK(23, 0)
#define K1_TSU_TIME_FILTER_PERIOD	GENMASK(21, 20)
#define K1_TSU_TIME_ADC_CNT_RST		GENMASK(7, 4)
#define K1_TSU_TIME_WAIT_REF_CNT	GENMASK(3, 0)

#define K1_TSU_PCTRL			0x00
#define K1_TSU_PCTRL_RAW_SEL		BIT(7)
#define K1_TSU_PCTRL_TEMP_MODE		BIT(3)
#define K1_TSU_PCTRL_ENABLE		BIT(0)

#define K1_TSU_PCTRL_SW_CTRL		GENMASK(21, 18)
#define K1_TSU_PCTRL_CTUNE		GENMASK(11, 8)
#define K1_TSU_PCTRL_HW_AUTO_MODE	BIT(23)

#define K1_TSU_PCTRL2			0x04
#define K1_TSU_PCTRL2_CLK_SEL_MASK	GENMASK(15, 14)
#define K1_TSU_PCTRL2_CLK_SEL_24M	(0 << 14)

struct k1_thermal_sensor {
	struct k1_thermal_priv *priv;
	struct thermal_zone_device *tzd;
	int id;
};

struct k1_thermal_priv {
	void __iomem *base;
	struct device *dev;
	struct clk *clk;
	struct clk *bus_clk;
	struct reset_control *reset;
	struct k1_thermal_sensor sensors[MAX_SENSOR_NUMBER];
};

static int k1_init_sensors(struct platform_device *pdev)
{
	struct k1_thermal_priv *priv = platform_get_drvdata(pdev);
	unsigned int temp;
	int i;

	/* Disable all the interrupts */
	writel(0xffffffff, priv->base + K1_TSU_INT_EN);

	/* Configure ADC sampling time and filter period */
	temp = readl(priv->base + K1_TSU_TIME);
	temp &= ~K1_TSU_TIME_MASK;
	temp |= K1_TSU_TIME_FILTER_PERIOD |
		K1_TSU_TIME_ADC_CNT_RST |
		K1_TSU_TIME_WAIT_REF_CNT;
	writel(temp, priv->base + K1_TSU_TIME);

	/*
	 * Enable all sensors' auto mode, enable dither control,
	 * consecutive mode, and power up sensor.
	 */
	temp = readl(priv->base + K1_TSU_PCTRL);
	temp |= K1_TSU_PCTRL_RAW_SEL |
		K1_TSU_PCTRL_TEMP_MODE |
		K1_TSU_PCTRL_HW_AUTO_MODE |
		K1_TSU_PCTRL_ENABLE;
	temp &= ~K1_TSU_PCTRL_SW_CTRL;
	temp &= ~K1_TSU_PCTRL_CTUNE;
	writel(temp, priv->base + K1_TSU_PCTRL);

	/* Select 24M clk for high speed mode */
	temp = readl(priv->base + K1_TSU_PCTRL2);
	temp &= ~K1_TSU_PCTRL2_CLK_SEL_MASK;
	temp |= K1_TSU_PCTRL2_CLK_SEL_24M;
	writel(temp, priv->base + K1_TSU_PCTRL2);

	/* Enable thermal interrupt */
	temp = readl(priv->base + K1_TSU_INT_EN);
	temp |= K1_TSU_INT_EN_MASK;
	writel(temp, priv->base + K1_TSU_INT_EN);

	/* Enable each sensor */
	for (i = 0; i < MAX_SENSOR_NUMBER; ++i) {
		temp = readl(priv->base + K1_TSU_EN);
		temp &= ~K1_TSU_EN_MASK(i);
		temp |= K1_TSU_EN_MASK(i);
		writel(temp, priv->base + K1_TSU_EN);
	}

	return 0;
}

static void k1_enable_sensor_irq(struct k1_thermal_sensor *sensor)
{
	struct k1_thermal_priv *priv = sensor->priv;
	unsigned int temp;

	temp = readl(priv->base + K1_TSU_INT_CLR);
	temp |= K1_TSU_INT_MASK(sensor->id);
	writel(temp, priv->base + K1_TSU_INT_CLR);

	temp = readl(priv->base + K1_TSU_INT_EN);
	temp &= ~K1_TSU_INT_MASK(sensor->id);
	writel(temp, priv->base + K1_TSU_INT_EN);
}

/*
 * The conversion formula used is:
 * T(m°C) = (((raw_value & mask) >> shift) - TEMPERATURE_OFFSET) * 1000
 */
static int k1_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct k1_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct k1_thermal_priv *priv = sensor->priv;

	*temp = readl(priv->base + K1_TSU_DATA(sensor->id));
	*temp &= K1_TSU_DATA_MASK(sensor->id);
	*temp >>= K1_TSU_DATA_SHIFT(sensor->id);

	*temp -= TEMPERATURE_OFFSET;

	*temp *= 1000;

	return 0;
}

/*
 * For each sensor, the hardware threshold register is 32 bits:
 * - Lower 16 bits [15:0] configure the low threshold temperature.
 * - Upper 16 bits [31:16] configure the high threshold temperature.
 */
static int k1_thermal_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct k1_thermal_sensor *sensor = thermal_zone_device_priv(tz);
	struct k1_thermal_priv *priv = sensor->priv;
	int high_code = high;
	int low_code = low;
	unsigned int temp;

	if (low >= high)
		return -EINVAL;

	if (low < 0)
		low_code = 0;

	high_code = high_code / 1000 + TEMPERATURE_OFFSET;
	temp = readl(priv->base + K1_TSU_THRSH(sensor->id));
	temp &= ~K1_TSU_THRSH_HIGH_MASK;
	temp |= (high_code << K1_TSU_THRSH_HIGH_SHIFT);
	writel(temp, priv->base + K1_TSU_THRSH(sensor->id));

	low_code = low_code / 1000 + TEMPERATURE_OFFSET;
	temp = readl(priv->base + K1_TSU_THRSH(sensor->id));
	temp &= ~K1_TSU_THRSH_LOW_MASK;
	temp |= (low_code << K1_TSU_THRSH_LOW_SHIFT);
	writel(temp, priv->base + K1_TSU_THRSH(sensor->id));

	return 0;
}

static const struct thermal_zone_device_ops k1_thermal_ops = {
	.get_temp = k1_thermal_get_temp,
	.set_trips = k1_thermal_set_trips,
};

static irqreturn_t k1_thermal_irq_thread(int irq, void *data)
{
	struct k1_thermal_priv *priv = (struct k1_thermal_priv *)data;
	int msk, status, i;

	status = readl(priv->base + K1_TSU_INT_STA);

	for (i = 0; i < MAX_SENSOR_NUMBER; i++) {
		if (status & K1_TSU_INT_MASK(i)) {
			msk = readl(priv->base + K1_TSU_INT_CLR);
			msk |= K1_TSU_INT_MASK(i);
			writel(msk, priv->base + K1_TSU_INT_CLR);
			/* Notify thermal framework to update trips */
			thermal_zone_device_update(priv->sensors[i].tzd, THERMAL_EVENT_UNSPECIFIED);
		}
	}

	return IRQ_HANDLED;
}

static int k1_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct k1_thermal_priv *priv;
	int i, irq, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->reset = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "Failed to get/deassert reset control\n");

	priv->clk = devm_clk_get_enabled(dev, "core");
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "Failed to get core clock\n");

	priv->bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(priv->bus_clk))
		return dev_err_probe(dev, PTR_ERR(priv->bus_clk),
				     "Failed to get bus clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = k1_init_sensors(pdev);

	for (i = 0; i < MAX_SENSOR_NUMBER; ++i) {
		priv->sensors[i].id = i;
		priv->sensors[i].priv = priv;
		priv->sensors[i].tzd = devm_thermal_of_zone_register(dev,
									i, priv->sensors + i,
									&k1_thermal_ops);
		if (IS_ERR(priv->sensors[i].tzd))
			return dev_err_probe(dev, PTR_ERR(priv->sensors[i].tzd),
						"Failed to register thermal zone: %d\n", i);

		/* Attach sysfs hwmon attributes for userspace monitoring */
		ret = devm_thermal_add_hwmon_sysfs(dev, priv->sensors[i].tzd);
		if (ret)
			dev_warn(dev, "Failed to add hwmon sysfs attributes\n");

		k1_enable_sensor_irq(priv->sensors + i);
	}

	ret = devm_request_threaded_irq(dev, irq, NULL,
					k1_thermal_irq_thread,
					IRQF_ONESHOT, "k1_thermal", priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	return 0;
}

static const struct of_device_id k1_thermal_dt_ids[] = {
	{ .compatible = "spacemit,k1-thermal" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, k1_thermal_dt_ids);

static struct platform_driver k1_thermal_driver = {
	.driver = {
		.name		= "k1_thermal",
		.of_match_table = k1_thermal_dt_ids,
	},
	.probe	= k1_thermal_probe,
};
module_platform_driver(k1_thermal_driver);

MODULE_DESCRIPTION("SpacemiT K1 Thermal Sensor Driver");
MODULE_AUTHOR("Shuwei Wu <shuweiwoo@163.com>");
MODULE_LICENSE("GPL");
