/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MFD_88PM886_H
#define __MFD_88PM886_H

#include <linux/i2c.h>
#include <linux/regmap.h>

#define PM886_A1_CHIP_ID		0xa1

#define PM886_IRQ_ONKEY			0

#define PM886_PAGE_OFFSET_REGULATORS	1
#define PM886_PAGE_OFFSET_GPADC		2

#define PM886_REG_ID			0x00

#define PM886_REG_STATUS1		0x01
#define PM886_ONKEY_STS1		BIT(0)

#define PM886_REG_INT_STATUS1		0x05

#define PM886_REG_INT_ENA_1		0x0a
#define PM886_INT_ENA1_ONKEY		BIT(0)

#define PM886_REG_MISC_CONFIG1		0x14
#define PM886_SW_PDOWN			BIT(5)

#define PM886_REG_MISC_CONFIG2		0x15
#define PM886_INT_INV			BIT(0)
#define PM886_INT_CLEAR			BIT(1)
#define PM886_INT_RC			0x00
#define PM886_INT_WC			BIT(1)
#define PM886_INT_MASK_MODE		BIT(2)

#define PM886_REG_RTC_CNT1		0xd1
#define PM886_REG_RTC_CNT2		0xd2
#define PM886_REG_RTC_CNT3		0xd3
#define PM886_REG_RTC_CNT4		0xd4
#define PM886_REG_RTC_SPARE1		0xea
#define PM886_REG_RTC_SPARE2		0xeb
#define PM886_REG_RTC_SPARE3		0xec
#define PM886_REG_RTC_SPARE4		0xed
#define PM886_REG_RTC_SPARE5		0xee
#define PM886_REG_RTC_SPARE6		0xef

#define PM886_REG_BUCK_EN		0x08
#define PM886_REG_LDO_EN1		0x09
#define PM886_REG_LDO_EN2		0x0a
#define PM886_REG_LDO1_VOUT		0x20
#define PM886_REG_LDO2_VOUT		0x26
#define PM886_REG_LDO3_VOUT		0x2c
#define PM886_REG_LDO4_VOUT		0x32
#define PM886_REG_LDO5_VOUT		0x38
#define PM886_REG_LDO6_VOUT		0x3e
#define PM886_REG_LDO7_VOUT		0x44
#define PM886_REG_LDO8_VOUT		0x4a
#define PM886_REG_LDO9_VOUT		0x50
#define PM886_REG_LDO10_VOUT		0x56
#define PM886_REG_LDO11_VOUT		0x5c
#define PM886_REG_LDO12_VOUT		0x62
#define PM886_REG_LDO13_VOUT		0x68
#define PM886_REG_LDO14_VOUT		0x6e
#define PM886_REG_LDO15_VOUT		0x74
#define PM886_REG_LDO16_VOUT		0x7a
#define PM886_REG_BUCK1_VOUT		0xa5
#define PM886_REG_BUCK2_VOUT		0xb3
#define PM886_REG_BUCK3_VOUT		0xc1
#define PM886_REG_BUCK4_VOUT		0xcf
#define PM886_REG_BUCK5_VOUT		0xdd

/* GPADC enable/disable registers */
#define PM886_REG_GPADC_CONFIG1		0x1
#define PM886_REG_GPADC_CONFIG2		0x2
#define PM886_REG_GPADC_CONFIG3		0x3
#define PM886_REG_GPADC_CONFIG6		0x6

/* GPADC bias current configuration registers */
#define PM886_REG_GPADC_CONFIG11	0xb
#define PM886_REG_GPADC_CONFIG12	0xc
#define PM886_REG_GPADC_CONFIG13	0xd
#define PM886_REG_GPADC_CONFIG14	0xe
#define PM886_REG_GPADC_CONFIG20	0x14

/* GPADC channel registers */
#define PM886_REG_GPADC_VSC		0x40
#define PM886_REG_GPADC_VCHG_PWR	0x4c
#define PM886_REG_GPADC_VCF_OUT		0x4e
#define PM886_REG_GPADC_TINT		0x50
#define PM886_REG_GPADC_GPADC0		0x54
#define PM886_REG_GPADC_GPADC1		0x56
#define PM886_REG_GPADC_GPADC2		0x58
#define PM886_REG_GPADC_VBAT		0xa0
#define PM886_REG_GPADC_GNDDET1		0xa4
#define PM886_REG_GPADC_GNDDET2		0xa6
#define PM886_REG_GPADC_VBUS		0xa8
#define PM886_REG_GPADC_GPADC3		0xaa
#define PM886_REG_GPADC_MIC_DET		0xac
#define PM886_REG_GPADC_VBAT_SLP	0xb0

#define PM886_LDO_VSEL_MASK		0x0f
#define PM886_BUCK_VSEL_MASK		0x7f

struct pm886_chip {
	struct i2c_client *client;
	unsigned int chip_id;
	struct regmap *regmap;
};
#endif /* __MFD_88PM886_H */
