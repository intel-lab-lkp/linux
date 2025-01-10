/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __MEDIA_I2C_DS90UB9XX_H__
#define __MEDIA_I2C_DS90UB9XX_H__

#include <linux/types.h>

#define UB953_REG_RESET_CTL			0x01
#define UB953_REG_RESET_CTL_DIGITAL_RESET_1	BIT(1)
#define UB953_REG_RESET_CTL_DIGITAL_RESET_0	BIT(0)

#define UB953_REG_IND_ACC_CTL			0xb0
#define UB953_REG_IND_ACC_ADDR			0xb1
#define UB953_REG_IND_ACC_DATA			0xb2

#define UB953_IND_TARGET_ANALOG			0x01

#define UB953_IND_ANA_TEMP_DYNAMIC_CFG		0x4b
#define UB953_IND_ANA_TEMP_DYNAMIC_CFG_OV	BIT(5)
#define UB953_IND_ANA_TEMP_STATIC_CFG		0x4c
#define UB953_IND_ANA_TEMP_STATIC_CFG_MASK	GENMASK(6, 4)

struct i2c_atr;

/**
 * struct ds90ub9xx_platform_data - platform data for FPD-Link Serializers.
 * @port: Deserializer RX port for this Serializer
 * @atr: I2C ATR
 * @bc_rate: back-channel clock rate
 */
struct ds90ub9xx_platform_data {
	u32 port;
	struct i2c_atr *atr;
	unsigned long bc_rate;
};

#endif /* __MEDIA_I2C_DS90UB9XX_H__ */
