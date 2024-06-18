/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * max77705_charger.h
 * Samsung max77705 Charger Header
 *
 * Copyright (C) 2015 Samsung Electronics, Inc.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __MAX77705_CHARGER_H
#define __MAX77705_CHARGER_H __FILE__

#include <linux/mfd/core.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/regulator/machine.h>

/* MAX77705_CHG_REG_CHG_INT */
#define MAX77705_BYP_I                  BIT(0)
#define MAX77705_INP_LIMIT_I		BIT(1)
#define MAX77705_BATP_I                 BIT(2)
#define MAX77705_BAT_I                  BIT(3)
#define MAX77705_CHG_I                  BIT(4)
#define MAX77705_WCIN_I                 BIT(5)
#define MAX77705_CHGIN_I                BIT(6)
#define MAX77705_AICL_I                 BIT(7)

/* MAX77705_CHG_REG_CHG_INT_MASK */
#define MAX77705_BYP_IM                 BIT(0)
#define MAX77705_INP_LIMIT_IM		BIT(1)
#define MAX77705_BATP_IM                BIT(2)
#define MAX77705_BAT_IM                 BIT(3)
#define MAX77705_CHG_IM                 BIT(4)
#define MAX77705_WCIN_IM                BIT(5)
#define MAX77705_CHGIN_IM               BIT(6)
#define MAX77705_AICL_IM                BIT(7)

/* MAX77705_CHG_REG_CHG_INT_OK */
#define MAX77705_BYP_OK                 BIT(0)
#define MAX77705_DISQBAT_OK		BIT(1)
#define MAX77705_BATP_OK		BIT(2)
#define MAX77705_BAT_OK                 BIT(3)
#define MAX77705_CHG_OK                 BIT(4)
#define MAX77705_WCIN_OK		BIT(5)
#define MAX77705_CHGIN_OK               BIT(6)
#define MAX77705_AICL_OK                BIT(7)

/* MAX77705_CHG_REG_CHG_DTLS_00 */
#define MAX77705_BATP_DTLS		BIT(0)
#define MAX77705_WCIN_DTLS		(BIT(3) | BIT(4))
#define MAX77705_WCIN_DTLS_SHIFT	3
#define MAX77705_CHGIN_DTLS             (BIT(5) | BIT(6))
#define MAX77705_CHGIN_DTLS_SHIFT       5

/* MAX77705_CHG_REG_CHG_DTLS_01 */
#define MAX77705_CHG_DTLS               (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define MAX77705_CHG_DTLS_SHIFT         0
#define MAX77705_BAT_DTLS               (BIT(4) | BIT(5) | BIT(6))
#define MAX77705_BAT_DTLS_SHIFT         4

/* MAX77705_CHG_REG_CHG_DTLS_02 */
#define MAX77705_BYP_DTLS               (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define MAX77705_BYP_DTLS_SHIFT         0

/* MAX77705_CHG_REG_CHG_CNFG_00 */
#define CHG_CNFG_00_MODE_SHIFT		        0
#define CHG_CNFG_00_CHG_SHIFT		        0
#define CHG_CNFG_00_UNO_SHIFT		        1
#define CHG_CNFG_00_OTG_SHIFT		        1
#define CHG_CNFG_00_BUCK_SHIFT		        2
#define CHG_CNFG_00_BOOST_SHIFT		        3
#define CHG_CNFG_00_WDTEN_SHIFT		        4
#define CHG_CNFG_00_MODE_MASK		        (0x0F << CHG_CNFG_00_MODE_SHIFT)
#define CHG_CNFG_00_CHG_MASK		        BIT(CHG_CNFG_00_CHG_SHIFT)
#define CHG_CNFG_00_UNO_MASK		        BIT(CHG_CNFG_00_UNO_SHIFT)
#define CHG_CNFG_00_OTG_MASK		        BIT(CHG_CNFG_00_OTG_SHIFT)
#define CHG_CNFG_00_BUCK_MASK		        BIT(CHG_CNFG_00_BUCK_SHIFT)
#define CHG_CNFG_00_BOOST_MASK		        BIT(CHG_CNFG_00_BOOST_SHIFT)
#define CHG_CNFG_00_WDTEN_MASK		        BIT(CHG_CNFG_00_WDTEN_SHIFT)
#define CHG_CNFG_00_UNO_CTRL			(CHG_CNFG_00_UNO_MASK | CHG_CNFG_00_BOOST_MASK)
#define CHG_CNFG_00_OTG_CTRL			(CHG_CNFG_00_OTG_MASK | CHG_CNFG_00_BOOST_MASK)

/* MAX77705_CHG_REG_CHG_CNFG_01 */
#define CHG_CNFG_01_FCHGTIME_SHIFT			0
#define CHG_CNFG_01_FCHGTIME_MASK			(0x7 << CHG_CNFG_01_FCHGTIME_SHIFT)
#define MAX77705_FCHGTIME_DISABLE			0x0

#define CHG_CNFG_01_CHG_RSTRT_SHIFT	4
#define CHG_CNFG_01_CHG_RSTRT_MASK	(0x3 << CHG_CNFG_01_CHG_RSTRT_SHIFT)
#define MAX77705_CHG_RSTRT_DISABLE	0x3

#define CHG_CNFG_01_PQEN_SHIFT			7
#define CHG_CNFG_01_PQEN_MASK			(0x1 << CHG_CNFG_01_PQEN_SHIFT)
#define MAX77705_CHG_PQEN_DISABLE		0x0
#define MAX77705_CHG_PQEN_ENABLE		0x1

/* MAX77705_CHG_REG_CHG_CNFG_02 */
#define CHG_CNFG_02_OTG_ILIM_SHIFT		6
#define CHG_CNFG_02_OTG_ILIM_MASK		(0x3 << CHG_CNFG_02_OTG_ILIM_SHIFT)
#define MAX77705_OTG_ILIM_500		0x0
#define MAX77705_OTG_ILIM_900		0x1
#define MAX77705_OTG_ILIM_1200		0x2
#define MAX77705_OTG_ILIM_1500		0x3
#define MAX77705_CHG_CC                         0x3F

/* MAX77705_CHG_REG_CHG_CNFG_03 */
#define CHG_CNFG_03_TO_ITH_SHIFT		0
#define CHG_CNFG_03_TO_ITH_MASK			(0x7 << CHG_CNFG_03_TO_ITH_SHIFT)
#define MAX77705_TO_ITH_150MA			0x0

#define CHG_CNFG_03_TO_TIME_SHIFT		3
#define CHG_CNFG_03_TO_TIME_MASK			(0x7 << CHG_CNFG_03_TO_TIME_SHIFT)
#define MAX77705_TO_TIME_30M			0x3

#define CHG_CNFG_03_SYS_TRACK_DIS_SHIFT		7
#define CHG_CNFG_03_SYS_TRACK_DIS_MASK		(0x1 << CHG_CNFG_03_SYS_TRACK_DIS_SHIFT)
#define MAX77705_SYS_TRACK_ENABLE	        0x0
#define MAX77705_SYS_TRACK_DISABLE	        0x1

/* MAX77705_CHG_REG_CHG_CNFG_04 */
#define MAX77705_CHG_MINVSYS_MASK               0xC0
#define MAX77705_CHG_MINVSYS_SHIFT		6
#define MAX77705_CHG_PRM_MASK                   0x1F
#define MAX77705_CHG_PRM_SHIFT                  0

#define CHG_CNFG_04_CHG_CV_PRM_SHIFT            0
#define CHG_CNFG_04_CHG_CV_PRM_MASK             (0x3F << CHG_CNFG_04_CHG_CV_PRM_SHIFT)

/* MAX77705_CHG_REG_CHG_CNFG_05 */
#define CHG_CNFG_05_REG_B2SOVRC_SHIFT	0
#define CHG_CNFG_05_REG_B2SOVRC_MASK	(0xF << CHG_CNFG_05_REG_B2SOVRC_SHIFT)
#define MAX77705_B2SOVRC_DISABLE	0x0
#define MAX77705_B2SOVRC_4_5A		0x6
#define MAX77705_B2SOVRC_4_8A		0x8
#define MAX77705_B2SOVRC_5_0A		0x9

/* MAX77705_CHG_CNFG_06 */
#define CHG_CNFG_01_WDTCLR_SHIFT		0
#define CHG_CNFG_01_WDTCLR_MASK			(0x3 << CHG_CNFG_01_WDTCLR_SHIFT)
#define MAX77705_WDTCLR				0x01
#define MAX77705_CHGPROT_MASK	(BIT(2) | BIT(3))
#define MAX77705_CHGPROT_UNLOCKED	(BIT(2) | BIT(3))
#define MAX77705_SLOWEST_LX_SLOPE	(BIT(5) | BIT(6))

/* MAX77705_CHG_REG_CHG_CNFG_07 */
#define MAX77705_CHG_FMBST			0x04
#define CHG_CNFG_07_REG_FMBST_SHIFT		2
#define CHG_CNFG_07_REG_FMBST_MASK		(0x1 << CHG_CNFG_07_REG_FMBST_SHIFT)
#define CHG_CNFG_07_REG_FGSRC_SHIFT		1
#define CHG_CNFG_07_REG_FGSRC_MASK		(0x1 << CHG_CNFG_07_REG_FGSRC_SHIFT)

/* MAX77705_CHG_REG_CHG_CNFG_08 */
#define CHG_CNFG_08_REG_FSW_SHIFT	0
#define CHG_CNFG_08_REG_FSW_MASK	(0x3 << CHG_CNFG_08_REG_FSW_SHIFT)
#define MAX77705_CHG_FSW_3MHz		0x00
#define MAX77705_CHG_FSW_2MHz		0x01
#define MAX77705_CHG_FSW_1_5MHz		0x02

/* MAX77705_CHG_REG_CHG_CNFG_09 */
#define MAX77705_CHG_CHGIN_LIM_MASK     0x7F
#define MAX77705_CHG_EN                         0x80

/* MAX77705_CHG_REG_CHG_CNFG_10 */
#define MAX77705_CHG_WCIN_LIM                   0x3F

/* MAX77705_CHG_REG_CHG_CNFG_11 */
#define CHG_CNFG_11_VBYPSET_SHIFT		0
#define CHG_CNFG_11_VBYPSET_MASK		(0x7F << CHG_CNFG_11_VBYPSET_SHIFT)

/* MAX77705_CHG_REG_CHG_CNFG_12 */
#define CHG_CNFG_12_CHGINSEL_SHIFT		5
#define MAX77705_CHG_WCINSEL			BIT(CHG_CNFG_12_CHGINSEL_SHIFT)
#define CHG_CNFG_12_CHGINSEL_MASK		BIT(CHG_CNFG_12_CHGINSEL_SHIFT)
#define CHG_CNFG_12_WCINSEL_SHIFT		6
#define CHG_CNFG_12_WCINSEL_MASK		BIT(CHG_CNFG_12_WCINSEL_SHIFT)
#define CHG_CNFG_12_VCHGIN_REG_MASK		(0x3 << 3)
#define CHG_CNFG_12_WCIN_REG_MASK		(0x3 << 1)
#define CHG_CNFG_12_REG_DISKIP_SHIFT		0
#define CHG_CNFG_12_REG_DISKIP_MASK		(0x1 << CHG_CNFG_12_REG_DISKIP_SHIFT)
#define MAX77705_DISABLE_SKIP			0x1
#define MAX77705_AUTO_SKIP			0x0

/* Convert current in mA to corresponding CNFG09 value */
inline u8 max77705_convert_ma_to_chgin_ilim_value(int cur)
{
	if (unlikely(cur < 0 && cur > 4000))
		return 0;
	return (((cur - 100) / 33) + 3);
}

/* Convert current in mA to corresponding CNFG10 value */
inline u8 max77705_convert_ma_to_wcin_ilim_value(int cur)
{
	if (unlikely(cur < 0 && cur > 4000))
		return 0;
	return (((cur - 60) / 20) + 3);
}

struct max77705_charger_data {
	struct device           *dev;
	struct mutex            charger_mutex;

	struct max77705_platform_data *max77705_pdata;
	struct max77705_dev	*max77705;
	struct power_supply_battery_info *bat_info;
	struct workqueue_struct *wqueue;
	struct work_struct	chgin_work;
	int		irq_chgin;

	struct power_supply	*psy_chg;

	int pmic_ver;
};

#endif /* __MAX77705_CHARGER_H */
