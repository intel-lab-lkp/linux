// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/delay.h>
#include <linux/pci.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_probe_helper.h>

#include <drm/drm_managed.h>

#include "yhgch_drm_drv.h"

#define GPIO_DATA		0x0802A0
#define GPIO_DATA_DIRECTION	0x0802A4

#define I2C_SCL_MASK		BIT(0)
#define I2C_SDA_MASK		BIT(1)

static void yhgch_set_i2c_signal(void *data, u32 mask, int value)
{
	struct yhgch_ddc *ddc = data;
	struct yhgch_drm_private *priv = ddc->priv;
	u32 tmp_dir = readl(priv->mmio + GPIO_DATA_DIRECTION);

	if (value) {
		tmp_dir |= mask;
		writel(tmp_dir, priv->mmio + GPIO_DATA_DIRECTION);
	} else {
		u32 tmp_data = readl(priv->mmio + GPIO_DATA);

		tmp_data &= ~mask;
		writel(tmp_data, priv->mmio + GPIO_DATA);

		tmp_dir &= ~mask;
		writel(tmp_dir, priv->mmio + GPIO_DATA_DIRECTION);
	}
}

static int yhgch_get_i2c_signal(void *data, u32 mask)
{
	struct yhgch_ddc *ddc = data;
	struct yhgch_drm_private *priv = ddc->priv;
	u32 tmp_dir = readl(priv->mmio + GPIO_DATA_DIRECTION);

	if (((~tmp_dir) & mask) != mask) {
		tmp_dir |= mask;
		writel(tmp_dir, priv->mmio + GPIO_DATA_DIRECTION);
	}

	return (readl(priv->mmio + GPIO_DATA) & mask) ? 1 : 0;
}

static void yhgch_ddc_setsda(void *data, int state)
{
	yhgch_set_i2c_signal(data, I2C_SDA_MASK, state);
}

static void yhgch_ddc_setscl(void *data, int state)
{
	yhgch_set_i2c_signal(data, I2C_SCL_MASK, state);
}

static int yhgch_ddc_getsda(void *data)
{
	return yhgch_get_i2c_signal(data, I2C_SDA_MASK);
}

static int yhgch_ddc_getscl(void *data)
{
	return yhgch_get_i2c_signal(data, I2C_SCL_MASK);
}

static void yhgch_ddc_release(struct drm_device *dev, void *res)
{
	struct yhgch_ddc *ddc = res;

	i2c_del_adapter(&ddc->adapter);
}

struct i2c_adapter *yhgch_ddc_create(struct yhgch_drm_private *priv)
{
	int ret = 0;
	struct yhgch_ddc *ddc;
	struct drm_device *dev = &priv->dev;

	ddc = drmm_kzalloc(dev, sizeof(struct yhgch_ddc), GFP_KERNEL);
	if (!ddc)
		return ERR_PTR(-ENOMEM);

	ddc->adapter.owner = THIS_MODULE;
	ddc->priv = priv;
	snprintf(ddc->adapter.name, I2C_NAME_SIZE, "INS i2c bit bus");
	ddc->adapter.dev.parent = priv->dev.dev;
	i2c_set_adapdata(&ddc->adapter, ddc);
	ddc->adapter.algo_data = &ddc->bit_data;

	ddc->bit_data.udelay = 20;
	ddc->bit_data.timeout = usecs_to_jiffies(2000);
	ddc->bit_data.data = ddc;
	ddc->bit_data.setsda = yhgch_ddc_setsda;
	ddc->bit_data.setscl = yhgch_ddc_setscl;
	ddc->bit_data.getsda = yhgch_ddc_getsda;
	ddc->bit_data.getscl = yhgch_ddc_getscl;

	ret = i2c_bit_add_bus(&ddc->adapter);
	if (ret)
		return ERR_PTR(ret);

	ret = drmm_add_action_or_reset(&priv->dev, yhgch_ddc_release, ddc);
	if (ret)
		return ERR_PTR(ret);

	return &ddc->adapter;
}
