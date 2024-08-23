// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2015 - 2024 Beijing WangXun Technology Co., Ltd. */

#include <linux/platform_data/i2c-wx.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/pci.h>

#include "i2c-designware-core.h"

#define I2C_DW_TXGBE_REQ_RETRY_CNT	4000
#define I2C_DW_TXGBE_MNG_SW		0x1E004
#define I2C_DW_TXGBE_MNG_SW_SM		BIT(0)
#define I2C_DW_TXGBE_FLUSH		0x10000

static int i2c_dw_txgbe_acquire_lock(struct dw_i2c_dev *dev)
{
	void __iomem *req_addr;
	u32 swsm;
	int i;

	req_addr = dev->ext + I2C_DW_TXGBE_MNG_SW;

	for (i = 0; i < I2C_DW_TXGBE_REQ_RETRY_CNT; i++) {
		writel(I2C_DW_TXGBE_MNG_SW_SM, req_addr);

		/* If we set the bit successfully then we got semaphore. */
		swsm = readl(req_addr);
		if (swsm & I2C_DW_TXGBE_MNG_SW_SM)
			break;

		udelay(50);
	}

	if (i == I2C_DW_TXGBE_REQ_RETRY_CNT)
		return -ETIMEDOUT;

	return 0;
}

static void i2c_dw_txgbe_release_lock(struct dw_i2c_dev *dev)
{
	writel(0, dev->ext + I2C_DW_TXGBE_MNG_SW);
	/* flush register status */
	readl(dev->ext + I2C_DW_TXGBE_FLUSH);
}

int i2c_dw_txgbe_probe_lock_support(struct dw_i2c_dev *dev)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	struct txgbe_i2c_platform_data *pdata;

	pdata = dev_get_platdata(&pdev->dev);
	if (!pdata)
		return -ENXIO;

	dev->ext = pdata->hw_addr;
	if (!dev->ext)
		return -ENXIO;

	dev->acquire_lock = i2c_dw_txgbe_acquire_lock;
	dev->release_lock = i2c_dw_txgbe_release_lock;

	return 0;
}
