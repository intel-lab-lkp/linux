// SPDX-License-Identifier: GPL-2.0

#include <linux/phy.h>
#include <linux/module.h>

#include <linux/regulator/driver.h>
#include <linux/of.h>
#include <linux/phylink.h>
#include <linux/sfp.h>

#include "qcom.h"

int at803x_debug_reg_read(struct phy_device *phydev, u16 reg)
{
	int ret;

	ret = phy_write(phydev, AT803X_DEBUG_ADDR, reg);
	if (ret < 0)
		return ret;

	return phy_read(phydev, AT803X_DEBUG_DATA);
}

int at803x_debug_reg_mask(struct phy_device *phydev, u16 reg,
			  u16 clear, u16 set)
{
	u16 val;
	int ret;

	ret = at803x_debug_reg_read(phydev, reg);
	if (ret < 0)
		return ret;

	val = ret & 0xffff;
	val &= ~clear;
	val |= set;

	return phy_write(phydev, AT803X_DEBUG_DATA, val);
}

int at803x_debug_reg_write(struct phy_device *phydev, u16 reg, u16 data)
{
	int ret;

	ret = phy_write(phydev, AT803X_DEBUG_ADDR, reg);
	if (ret < 0)
		return ret;

	return phy_write(phydev, AT803X_DEBUG_DATA, data);
}
