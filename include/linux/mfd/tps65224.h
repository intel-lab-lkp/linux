/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Functions to access TPS65224 Power Management IC
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __LINUX_MFD_TPS65224_H
#define __LINUX_MFD_TPS65224_H

#include <linux/device.h>
#include <linux/regmap.h>

struct regmap_irq_chip_data;

/* Macro to get page index from register address */
#define TPS65224_REG_TO_PAGE(reg)	((reg) >> 8)

/* Registers for page 0 of TPS65224 */
#define TPS65224_REG_DEV_REV				0x01

#define TPS65224_REG_NVM_CODE_1				0x02
#define TPS65224_REG_NVM_CODE_2				0x03

#define TPS65224_REG_BUCKX_CTRL(buck_inst)		(0x04 + ((buck_inst) << 1))
#define TPS65224_REG_BUCKX_CONF(buck_inst)		(0x05 + ((buck_inst) << 1))
#define TPS65224_REG_BUCKX_VOUT(buck_inst)		(0x0e + ((buck_inst) << 1))
#define TPS65224_REG_BUCKX_PG_WINDOW(buck_inst)		(0x18 + (buck_inst))

#define TPS65224_REG_LDOX_CTRL(ldo_inst)			(0x1d + (ldo_inst))
#define TPS65224_REG_LDOX_VOUT(ldo_inst)		(0x23 + (ldo_inst))
#define TPS65224_REG_LDOX_PG_WINDOW(ldo_inst)		(0x27 + (ldo_inst))

#define TPS65224_REG_VCCA_VMON_CTRL			0x2b
#define TPS65224_REG_VCCA_PG_WINDOW			0x2c
#define TPS65224_REG_VMON1_PG_WINDOW			0x2d
#define TPS65224_REG_VMON1_PG_LEVEL			0x2e
#define TPS65224_REG_VMON2_PG_WINDOW			0x2f
#define TPS65224_REG_VMON2_PG_LEVEL			0x30

#define TPS65224_REG_GPIOX_CONF(gpio_inst)		(0x31 + (gpio_inst))
#define TPS65224_REG_POWER_ON_CONFIG			0x3c
#define TPS65224_REG_GPIO_OUT_1				0x3d
#define TPS65224_REG_GPIO_IN_1				0x3f

#define TPS65224_REG_RAIL_SEL_1				0x41
#define TPS65224_REG_RAIL_SEL_2				0x42
#define TPS65224_REG_RAIL_SEL_3				0x43

#define TPS65224_REG_FSM_TRIG_SEL_1				0x44
#define TPS65224_REG_FSM_TRIG_SEL_2			0x45
#define TPS65224_REG_FSM_TRIG_MASK_1			0x46
#define TPS65224_REG_FSM_TRIG_MASK_2			0x47

#define TPS65224_REG_MASK_BUCK				0x49
#define TPS65224_REG_MASK_LDO_VMON			0x4c
#define TPS65224_REG_MASK_GPIO_FALL			0x4f
#define TPS65224_REG_MASK_GPIO_RISE			0x50
#define TPS65224_REG_MASK_STARTUP			0x52
#define TPS65224_REG_MASK_MISC				0x53
#define TPS65224_REG_MASK_MODERATE_ERR			0x54
#define TPS65224_REG_MASK_FSM_ERR			0x56
#define TPS65224_REG_MASK_ESM				0x59

#define TPS65224_REG_INT_TOP				0x5a
#define TPS65224_REG_INT_BUCK				0x5b
#define TPS65224_REG_INT_LDO_VMON			0x5f
#define TPS65224_REG_INT_GPIO				0x63
#define TPS65224_REG_INT_STARTUP			0x65
#define TPS65224_REG_INT_MISC				0x66
#define TPS65224_REG_INT_MODERATE_ERR			0x67
#define TPS65224_REG_INT_SEVERE_ERR			0x68
#define TPS65224_REG_INT_FSM_ERR			0x69
#define TPS65224_REG_INT_ESM				0x6c

#define TPS65224_REG_STAT_BUCK				0x6d
#define TPS65224_REG_STAT_LDO_VMON			0x70
#define TPS65224_REG_STAT_STARTUP			0x73
#define TPS65224_REG_STAT_MISC				0x74
#define TPS65224_REG_STAT_MODERATE_ERR			0x75
#define TPS65224_REG_STAT_SEVERE_ERR			0x76

#define TPS65224_REG_PLL_CTRL				0x7c

#define TPS65224_REG_CONFIG_1				0x7d
#define TPS65224_REG_CONFIG_2				0x7e

#define TPS65224_REG_ENABLE_DRV_REG			0x80

#define TPS65224_REG_MISC_CTRL				0x81

#define TPS65224_REG_ENABLE_DRV_STAT			0x82

#define TPS65224_REG_RECOV_CNT_REG_1			0x83
#define TPS65224_REG_RECOV_CNT_REG_2			0x84

#define TPS65224_REG_FSM_I2C_TRIGGERS			0x85
#define TPS65224_REG_FSM_NSLEEP_TRIGGERS		0x86

#define TPS65224_REG_BUCK_RESET_REG			0x87

#define TPS65224_REG_SPREAD_SPECTRUM_1			0x88

#define TPS65224_REG_FSM_STEP_SIZE			0x8b

#define TPS65224_REG_USER_SPARE_REGS			0x8e

#define TPS65224_REG_ESM_MCU_START_REG			0x8f
#define TPS65224_REG_ESM_MCU_DELAY1_REG			0x90
#define TPS65224_REG_ESM_MCU_DELAY2_REG			0x91
#define TPS65224_REG_ESM_MCU_MODE_CFG			0x92
#define TPS65224_REG_ESM_MCU_HMAX_REG			0x93
#define TPS65224_REG_ESM_MCU_HMIN_REG			0x94
#define TPS65224_REG_ESM_MCU_LMAX_REG			0x95
#define TPS65224_REG_ESM_MCU_LMIN_REG			0x96
#define TPS65224_REG_ESM_MCU_ERR_CNT_REG		0x97

#define TPS65224_REG_REGISTER_LOCK			0xa1

/* Added Additional reg's as per datasheet start*/
#define TPS65224_REG_SRAM_ACCESS_1                      0xa2
#define TPS65224_REG_SRAM_ACCESS_2                      0xa3
#define TPS65224_REG_SRAM_ADDR_CTRL                     0xa4
#define TPS65224_REG_RECOV_CNT_PFSM_INCR                0xa5
/* Added Additional reg's as per datasheet end*/

#define TPS65224_REG_MANUFACTURING_VER			0xa6

#define TPS65224_REG_CUSTOMER_NVM_ID_REG		0xa7

#define TPS65224_REG_SOFT_REBOOT_REG			0xab

/* Added Additional reg's as per datasheet start*/
#define TPS65224_REG_ADC_CTRL                           0xac
#define TPS65224_REG_ADC_RESULT_REG_1                   0xad
#define TPS65224_REG_ADC_RESULT_REG_2                   0xae
#define TPS65224_REG_STARTUP_CTRL                       0xc3

#define TPS65224_REG_ADC_GAIN_COMP_REG                  0xd0

#define TPS65224_REG_CRC_CALC_CONTROL                   0xef
#define TPS65224_REG_REGMAP_USER_CRC_LOW                0xf0
#define TPS65224_REG_REGMAP_USER_CRC_HIGH               0xf1
/* Added Additional reg's as per datasheet end*/

#define TPS65224_REG_SCRATCH_PAD_REG_1			0xc9
#define TPS65224_REG_SCRATCH_PAD_REG_2			0xca
#define TPS65224_REG_SCRATCH_PAD_REG_3			0xcb
#define TPS65224_REG_SCRATCH_PAD_REG_4			0xcc

#define TPS65224_REG_PFSM_DELAY_REG_1			0xcd
#define TPS65224_REG_PFSM_DELAY_REG_2			0xce
#define TPS65224_REG_PFSM_DELAY_REG_3			0xcf

/* Registers for page 4 of TPS65224 */
#define TPS65224_REG_WD_ANSWER_REG			0x401
#define TPS65224_REG_WD_QUESTION_ANSW_CNT		0x402
#define TPS65224_REG_WD_WIN1_CFG			0x403
#define TPS65224_REG_WD_WIN2_CFG			0x404
#define TPS65224_REG_WD_LONGWIN_CFG			0x405
#define TPS65224_REG_WD_MODE_REG			0x406
#define TPS65224_REG_WD_QA_CFG				0x407
#define TPS65224_REG_WD_ERR_STATUS			0x408
#define TPS65224_REG_WD_THR_CFG				0x409
#define TPS65224_REG_WD_FAIL_CNT_REG			0x40a

/* BUCKX_CTRL register field definition */
#define TPS65224_BIT_BUCK_EN				BIT(0)
#define TPS65224_BIT_BUCK_FPWM				BIT(1)
#define TPS65224_BIT_BUCK_VMON_EN			BIT(4)
#define TPS65224_BIT_BUCK_PLDN				BIT(5)

/* BUCKX_CONF register field definition */
#define TPS65224_MASK_BUCK_SLEW_RATE			GENMASK(1, 0)

/* BUCKX_PG_WINDOW register field definition */
#define TPS65224_MASK_BUCK_VMON_THR                     GENMASK(1, 0)

/* BUCKX VSET */
#define TPS65224_MASK_BUCK1_VSET			GENMASK(7, 0)
#define TPS65224_MASK_BUCKS_VSET			GENMASK(6, 0)

/* LDOX_CTRL register field definition */
#define TPS65224_BIT_LDO_EN				BIT(0)
#define TPS65224_BIT_LDO_VMON_EN			BIT(4)
#define TPS65224_BIT_LDO_DISCHARGE_EN				BIT(5)

/* LDOX_PG_WINDOW register field definition */
#define TPS65224_MASK_LDO_VMON_THR			GENMASK(1, 0)

/* VCCA_VMON_CTRL register field definition */
#define TPS65224_BIT_VMON_EN				BIT(0)
#define TPS65224_BIT_VMON1_EN				BIT(1)
#define TPS65224_BIT_VMON2_EN				BIT(3)
#define TPS65224_MASK_VMON_DEGLITCH_SEL			GENMASK(7, 5)

/* LDOX_VOUT register field definition */
#define TPS65224_MASK_LDO_VSET                          GENMASK(6, 1)
#define TPS65224_BIT_LDO_BYP_CONFIG                         BIT(7)

/* VCCA_PG_WINDOW register field definition */
#define TPS65224_MASK_VCCA_VMON_THR			GENMASK(1, 0)
#define TPS65224_BIT_VCCA_PG_SET			BIT(6)

/* VMONX_PG_WINDOW register field definition */
#define TPS65224_MASK_VMONX_THR			GENMASK(1, 0)

/* GPIO_CONF register field definition */
#define TPS65224_BIT_GPIO_DIR				BIT(0)
#define TPS65224_BIT_GPIO_OD				BIT(1)
#define TPS65224_BIT_GPIO_PU_SEL			BIT(2)
#define TPS65224_BIT_GPIO_PU_PD_EN			BIT(3)
#define TPS65224_BIT_GPIO_DEGLITCH_EN			BIT(4)
#define TPS65224_MASK_GPIO_SEL				GENMASK(6, 5)
#define TPS65224_MASK_GPIO_SEL_GPIO6			GENMASK(7, 5)

/* POWER_ON_CONFIG register field definition */
#define TPS65224_BIT_NINT_ENDRV_PU_SEL			BIT(0)
#define TPS65224_BIT_NINT_ENDRV_SEL			BIT(1)
#define TPS65224_BIT_EN_PB_DEGL				BIT(5)
#define TPS65224_MASK_EN_PB_VSENSE_CONFIG		GENMASK(7, 6)

/* GPIO_OUT_X register field definition */
#define TPS65224_BIT_GPIOX_OUT(gpio_inst)		BIT((gpio_inst))

/* GPIO_IN_X register field definition */
#define TPS65224_BIT_GPIOX_IN(gpio_inst)		BIT((gpio_inst))


/* RAIL_SEL_1 register field definition */
#define TPS65224_MASK_BUCK1_GRP_SEL			GENMASK(1, 0)
#define TPS65224_MASK_BUCK2_GRP_SEL			GENMASK(3, 2)
#define TPS65224_MASK_BUCK3_GRP_SEL			GENMASK(5, 4)
#define TPS65224_MASK_BUCK4_GRP_SEL			GENMASK(7, 6)

/* RAIL_SEL_2 register field definition */
#define TPS65224_MASK_LDO1_GRP_SEL			GENMASK(3, 2)
#define TPS65224_MASK_LDO2_GRP_SEL			GENMASK(5, 4)
#define TPS65224_MASK_LDO3_GRP_SEL			GENMASK(7, 6)

/* RAIL_SEL_3 register field definition */
#define TPS65224_MASK_VCCA_GRP_SEL			GENMASK(3, 2)
#define TPS65224_MASK_VMON1_GRP_SEL			GENMASK(5, 4)
#define TPS65224_MASK_VMON2_GRP_SEL			GENMASK(7, 6)

/* FSM_TRIG_SEL_1 register field definition */
#define TPS65224_MASK_MCU_RAIL_TRIG			GENMASK(1, 0)
#define TPS65224_MASK_SOC_RAIL_TRIG			GENMASK(3, 2)
#define TPS65224_MASK_OTHER_RAIL_TRIG			GENMASK(5, 4)
#define TPS65224_MASK_SEVERE_ERR_TRIG			GENMASK(7, 6)

/* FSM_TRIG_SEL_2 register field definition */
#define TPS65224_MASK_MODERATE_ERR_TRIG			GENMASK(1, 0)

/* FSM_TRIG_MASK_X register field definition */
#define TPS65224_BIT_GPIOX_FSM_MASK(gpio_inst)		BIT(((gpio_inst) << 1) % 8)
#define TPS65224_BIT_GPIOX_FSM_MASK_POL(gpio_inst)	BIT(((gpio_inst) << 1) % 8 + 1)

/* MASK_BUCK Register field definition */
#define TPS65224_BIT_BUCK1_UVOV_MASK                    BIT(0)
#define TPS65224_BIT_BUCK2_UVOV_MASK			BIT(1)
#define TPS65224_BIT_BUCK3_UVOV_MASK			BIT(2)
#define TPS65224_BIT_BUCK4_UVOV_MASK			BIT(4)

/* MASK_LDO_VMON register field definition */
#define TPS65224_BIT_LDO1_UVOV_MASK                     BIT(0)
#define TPS65224_BIT_LDO2_UVOV_MASK			BIT(1)
#define TPS65224_BIT_LDO3_UVOV_MASK			BIT(2)
#define TPS65224_BIT_VCCA_UVOV_MASK			BIT(4)
#define TPS65224_BIT_VMON1_UVOV_MASK			BIT(5)
#define TPS65224_BIT_VMON2_UVOV_MASK			BIT(6)

/* MASK_GPIOX register field definition */
#define TPS65224_BIT_GPIOX_FALL_MASK(gpio_inst)		BIT((gpio_inst))
#define TPS65224_BIT_GPIOX_RISE_MASK(gpio_inst)		BIT((gpio_inst))

/* MASK_STARTUP register field definition */
#define TPS65224_BIT_VSENSE_MASK                        BIT(0)
#define TPS65224_BIT_ENABLE_MASK			BIT(1)
#define TPS65224_BIT_PB_SHORT_MASK                      BIT(2)
#define TPS65224_BIT_FSD_MASK				BIT(4)
#define TPS65224_BIT_SOFT_REBOOT_MASK			BIT(5)

/* MASK_MISC register field definition */
#define TPS65224_BIT_BIST_PASS_MASK			BIT(0)
#define TPS65224_BIT_EXT_CLK_MASK			BIT(1)
#define TPS65224_BIT_REG_UNLOCK_MASK                    BIT(2)
#define TPS65224_BIT_TWARN_MASK				BIT(3)
#define TPS65224_BIT_PB_LONG_MASK                       BIT(4)
#define TPS65224_BIT_PB_FALL_MASK                       BIT(5)
#define TPS65224_BIT_PB_RISE_MASK                       BIT(6)
#define TPS65224_BIT_ADC_CONV_READY_MASK                BIT(7)

/* MASK_MODERATE_ERR register field definition */
#define TPS65224_BIT_BIST_FAIL_MASK			BIT(1)
#define TPS65224_BIT_REG_CRC_ERR_MASK			BIT(2)

/* MASK_FSM_ERR register field definition */
#define TPS65224_BIT_IMM_SHUTDOWN_MASK			BIT(0)
#define TPS65224_BIT_ORD_SHUTDOWN_MASK			BIT(1)
#define TPS65224_BIT_MCU_PWR_ERR_MASK			BIT(2)
#define TPS65224_BIT_SOC_PWR_ERR_MASK			BIT(3)
#define TPS65224_BIT_COMM_ERR_MASK                      BIT(4)
#define TPS65224_BIT_I2C2_ERR_MASK                      BIT(5)

/* MASK_ESM register field definition */
#define TPS65224_BIT_ESM_MCU_PIN_MASK			BIT(3)
#define TPS65224_BIT_ESM_MCU_FAIL_MASK			BIT(4)
#define TPS65224_BIT_ESM_MCU_RST_MASK			BIT(5)

/* INT_TOP register field definition */
#define TPS65224_BIT_BUCK_INT				BIT(0)
#define TPS65224_BIT_LDO_VMON_INT			BIT(1)
#define TPS65224_BIT_GPIO_INT				BIT(2)
#define TPS65224_BIT_STARTUP_INT			BIT(3)
#define TPS65224_BIT_MISC_INT				BIT(4)
#define TPS65224_BIT_MODERATE_ERR_INT			BIT(5)
#define TPS65224_BIT_SEVERE_ERR_INT			BIT(6)
#define TPS65224_BIT_FSM_ERR_INT			BIT(7)

/* INT_BUCK register field definition */
#define TPS65224_BIT_BUCK1_UVOV_INT			BIT(0)
#define TPS65224_BIT_BUCK2_UVOV_INT			BIT(1)
#define TPS65224_BIT_BUCK3_UVOV_INT                     BIT(2)
#define TPS65224_BIT_BUCK4_UVOV_INT                     BIT(3)

/* INT_LDO_VMON register field definition */
#define TPS65224_BIT_LDO1_UVOV_INT			BIT(0)
#define TPS65224_BIT_LDO2_UVOV_INT			BIT(1)
#define TPS65224_BIT_LDO3_UVOV_INT			BIT(2)
#define TPS65224_BIT_VCCA_UVOV_INT                      BIT(4)
#define TPS65224_BIT_VMON1_UVOV_INT                     BIT(5)
#define TPS65224_BIT_VMON2_UVOV_INT                     BIT(6)

/* INT_GPIO register field definition */
#define TPS65224_BIT_GPIO1_INT				BIT(0)
#define TPS65224_BIT_GPIO2_INT                          BIT(1)
#define TPS65224_BIT_GPIO3_INT                          BIT(2)
#define TPS65224_BIT_GPIO4_INT                          BIT(3)
#define TPS65224_BIT_GPIO5_INT                          BIT(4)
#define TPS65224_BIT_GPIO6_INT                          BIT(5)

/* INT_STARTUP register field definition */
#define TPS65224_BIT_VSENSE_INT			        BIT(0)
#define TPS65224_BIT_ENABLE_INT				BIT(1)
#define TPS65224_BIT_PB_SHORT_INT			BIT(2)
#define TPS65224_BIT_FSD_INT                            BIT(4)
#define TPS65224_BIT_SOFT_REBOOT_INT			BIT(5)

/* INT_MISC register field definition */
#define TPS65224_BIT_BIST_PASS_INT			BIT(0)
#define TPS65224_BIT_EXT_CLK_INT			BIT(1)
#define TPS65224_BIT_REG_UNLOCK_INT                     BIT(2)
#define TPS65224_BIT_TWARN_INT                          BIT(3)
#define TPS65224_BIT_PB_LONG_INT                        BIT(4)
#define TPS65224_BIT_PB_FALL_INT                        BIT(5)
#define TPS65224_BIT_PB_RISE_INT                        BIT(6)
#define TPS65224_BIT_ADC_CONV_READY_INT                 BIT(7)

/* INT_MODERATE_ERR register field definition */
#define TPS65224_BIT_TSD_ORD_INT			BIT(0)
#define TPS65224_BIT_BIST_FAIL_INT			BIT(1)
#define TPS65224_BIT_REG_CRC_ERR_INT			BIT(2)
#define TPS65224_BIT_RECOV_CNT_INT			BIT(3)

/* INT_SEVERE_ERR register field definition */
#define TPS65224_BIT_TSD_IMM_INT			BIT(0)
#define TPS65224_BIT_VCCA_OVP_INT			BIT(1)
#define TPS65224_BIT_PFSM_ERR_INT			BIT(2)
#define TPS65224_BIT_BG_XMON_INT                        BIT(3)

/* INT_FSM_ERR register field definition */
#define TPS65224_BIT_IMM_SHUTDOWN_INT			BIT(0)
#define TPS65224_BIT_ORD_SHUTDOWN_INT			BIT(1)
#define TPS65224_BIT_MCU_PWR_ERR_INT			BIT(2)
#define TPS65224_BIT_SOC_PWR_ERR_INT			BIT(3)
#define TPS65224_BIT_COMM_ERR_INT			BIT(4)
#define TPS65224_BIT_I2C2_ERR_INT			BIT(5)
#define TPS65224_BIT_ESM_INT				BIT(6)
#define TPS65224_BIT_WD_INT				BIT(7)

/* INT_ESM register field definition */
#define TPS65224_BIT_ESM_MCU_PIN_INT			BIT(3)
#define TPS65224_BIT_ESM_MCU_FAIL_INT			BIT(4)
#define TPS65224_BIT_ESM_MCU_RST_INT			BIT(5)

/* STAT_LDO_VMON register field definition */
#define TPS65224_BIT_LDO1_UVOV_STAT			BIT(0)
#define TPS65224_BIT_LDO2_UVOV_STAT			BIT(1)
#define TPS65224_BIT_LDO3_UVOV_STAT			BIT(2)
#define TPS65224_BIT_VCCA_UVOV_STAT			BIT(4)
#define TPS65224_BIT_VMON1_UVOV_STAT                    BIT(5)
#define TPS65224_BIT_VMON2_UVOV_STAT                    BIT(6)

/* STAT_STARTUP register field definition */
#define TPS65224_BIT_VSENSE_STAT                        BIT(0)
#define TPS65224_BIT_ENABLE_STAT			BIT(1)
#define TPS65224_BIT_PB_LEVEL_STAT                      BIT(2)

/* STAT_MISC register field definition */
#define TPS65224_BIT_EXT_CLK_STAT			BIT(1)
#define TPS65224_BIT_TWARN_STAT				BIT(3)

/* STAT_MODERATE_ERR register field definition */
#define TPS65224_BIT_TSD_ORD_STAT			BIT(0)

/* STAT_SEVERE_ERR register field definition */
#define TPS65224_BIT_TSD_IMM_STAT			BIT(0)
#define TPS65224_BIT_VCCA_OVP_STAT                      BIT(1)
#define TPS65224_BIT_BG_XMON_STAT			BIT(3)

/* PLL_CTRL register field definition */
#define TPS65224_MASK_EXT_CLK_FREQ			GENMASK(1, 0)

/* CONFIG_1 register field definition */
#define TPS65224_BIT_TWARN_LEVEL			BIT(0)
#define TPS65224_BIT_TSD_ORD_LEVEL			BIT(1)
#define TPS65224_BIT_I2C1_HS				BIT(3)
#define TPS65224_BIT_I2C2_HS				BIT(4)
#define TPS65224_BIT_NSLEEP1_MASK			BIT(6)
#define TPS65224_BIT_NSLEEP2_MASK			BIT(7)

/* CONFIG_2 register field definition */
#define TPS65224_BIT_I2C1_SPI_CRC_EN			BIT(4)
#define TPS65224_BIT_I2C2_CRC_EN			BIT(5)

/* ENABLE_DRV_REG register field definition */
#define TPS65224_BIT_ENABLE_DRV				BIT(0)

/* MISC_CTRL register field definition */
#define TPS65224_BIT_NRSTOUT				BIT(0)
#define TPS65224_BIT_LPM_EN			        BIT(2)
#define TPS65224_BIT_SEL_EXT_CLK			BIT(5)

/* ENABLE_DRV_STAT register field definition */
#define TPS65224_BIT_EN_DRV_IN				BIT(0)
#define TPS65224_BIT_NRSTOUT_IN				BIT(1)
#define TPS65224_BIT_FORCE_EN_DRV_LOW			BIT(3)
#define TPS65224_BIT_TSD_DISABLE			BIT(5)

/* RECOV_CNT_REG_1 register field definition */
#define TPS65224_MASK_RECOV_CNT				GENMASK(3, 0)

/* RECOV_CNT_REG_2 register field definition */
#define TPS65224_MASK_RECOV_CNT_THR			GENMASK(3, 0)
#define TPS65224_BIT_RECOV_CNT_CLR			BIT(4)

/* FSM_I2C_TRIGGERS register field definition */
#define TPS65224_BIT_TRIGGER_I2C(bit)			BIT(bit)

/* FSM_NSLEEP_TRIGGERS register field definition */
#define TPS65224_BIT_NSLEEP1B				BIT(0)
#define TPS65224_BIT_NSLEEP2B				BIT(1)

/* BUCK_RESET_REG register field definition */
#define TPS65224_BIT_BUCKX_RESET(buck_inst)		BIT(buck_inst)

/* SPREAD_SPECTRUM_1 register field definition */
#define TPS65224_BIT_SS_DEPTH				BIT(0)
#define TPS65224_BIT_SS_EN				BIT(2)

/* FSM_STEP_SIZE register field definition */
#define TPS65224_MASK_PFSM_DELAY_STEP			GENMASK(4, 0)

/* USER_SPARE_REGS register field definition */
#define TPS65224_BIT_USER_SPARE(bit)			BIT(bit)

/* ESM_MCU_START_REG register field definition */
#define TPS65224_BIT_ESM_MCU_START			BIT(0)

/* ESM_MCU_MODE_CFG register field definition */
#define TPS65224_MASK_ESM_MCU_ERR_CNT_TH		GENMASK(3, 0)
#define TPS65224_BIT_ESM_MCU_ENDRV			BIT(5)
#define TPS65224_BIT_ESM_MCU_EN				BIT(6)
#define TPS65224_BIT_ESM_MCU_MODE			BIT(7)

/* ESM_MCU_ERR_CNT_REG register field definition */
#define TPS65224_MASK_ESM_MCU_ERR_CNT			GENMASK(4, 0)

/* REGISTER_LOCK register field definition */
#define TPS65224_BIT_REGISTER_LOCK_STATUS		BIT(0)

/* SRAM_ACCESS_1 Register field definition */
#define TPS65224_MASk_SRAM_UNLOCK_SEQ		        GENMASK(7, 0)

/* SRAM_ACCESS_2 Register field definition */
#define TPS65224_BIT_SRAM_WRITE_MODE			BIT(0)
#define TPS65224_BIT_OTP_PROG_USER			BIT(1)
#define TPS65224_BIT_OTP_PROG_PFSM			BIT(2)
#define TPS65224_BIT_OTP_PROG_STATUS			BIT(3)
#define TPS65224_BIT_SRAM_UNLOCKED			BIT(6)
#define TPS65224_USER_PROG_ALLOWED		        BIT(7)

/* SRAM_ADDR_CTRL Register field definition */
#define TPS65224_MASk_SRAM_SEL				GENMASK(1, 0)

/* RECOV_CNT_PFSM_INCR Register field definition */
#define TPS65224_BIT_INCREMENT_RECOV_CNT		BIT(0)

/* MANUFACTURING_VER Register field definition */
#define TPS65224_MASk_SILICON_REV			GENMASK(7, 0)

/* CUSTOMER_NVM_ID_REG Register field definition */
#define TPS65224_MASk_CUSTOMER_NVM_ID			GENMASK(7, 0)

/* SOFT_REBOOT_REG register field definition */
#define TPS65224_BIT_SOFT_REBOOT			BIT(0)

/* ADC_CTRL Register field definition */
#define TPS65224_BIT_ADC_START				BIT(0)
#define TPS65224_BIT_ADC_CONT_CONV			BIT(1)
#define TPS65224_BIT_ADC_THERMAL_SEL                    BIT(2)
#define TPS65224_BIT_ADC_RDIV_EN                        BIT(3)
#define TPS65224_BIT_ADC_STATUS				BIT(7)

/* ADC_RESULT_REG_1 Register field definition */
#define TPS65224_MASk_ADC_RESULT_11_4			GENMASK(7, 0)

/* ADC_RESULT_REG_2 Register field definition */
#define TPS65224_MASk_ADC_RESULT_3_0		        GENMASK(7, 4)

/* STARTUP_CTRL Register field definition */
#define TPS65224_MASk_STARTUP_DEST		        GENMASK(6, 5)
#define TPS65224_BIT_FIRST_STARTUP_DONE		        BIT(7)

/* SCRATCH_PAD_REG_1 Register field definition */
#define TPS65224_MASk_SCRATCH_PAD_1		        GENMASK(7, 0)

/* SCRATCH_PAD_REG_2 Register field definition */
#define TPS65224_MASk_SCRATCH_PAD_2		        GENMASK(7, 0)

/* SCRATCH_PAD_REG_3 Register field definition */
#define TPS65224_MASk_SCRATCH_PAD_3		        GENMASK(7, 0)

/* SCRATCH_PAD_REG_4 Register field definition */
#define TPS65224_MASk_SCRATCH_PAD_4		        GENMASK(7, 0)

/* PFSM_DELAY_REG_1 Register field definition */
#define TPS65224_MASk_PFSM_DELAY1		        GENMASK(7, 0)

/* PFSM_DELAY_REG_2 Register field definition */
#define TPS65224_MASk_PFSM_DELAY2		        GENMASK(7, 0)

/* PFSM_DELAY_REG_3 Register field definition */
#define TPS65224_MASk_PFSM_DELAY3		        GENMASK(7, 0)

/* CRC_CALC_CONTROL Register field definition */
#define TPS65224_BIT_RUN_CRC_BIST		        BIT(0)
#define TPS65224_BIT_RUN_CRC_UPDATE		        BIT(1)

/* ADC_GAIN_COMP_REG Register field definition */
#define TPS65224_MASk_ADC_GAIN_COMP		        GENMASK(7, 0)

/* REGMAP_USER_CRC_LOW Register field definition */
#define TPS65224_MASk_REGMAP_USER_CRC16_LOW	        GENMASK(7, 0)

/* REGMAP_USER_CRC_HIGH Register field definition */
#define TPS65224_MASk_REGMAP_USER_CRC16_HIGH	        GENMASK(7, 0)

/* WD_ANSWER_REG Register field definition */
#define TPS65224_MASk_WD_ANSWER				GENMASK(7, 0)

/* WD_QUESTION_ANSW_CNT register field definition */
#define TPS65224_MASK_WD_QUESTION			GENMASK(3, 0)
#define TPS65224_MASK_WD_ANSW_CNT			GENMASK(5, 4)
#define TPS65224_BIT_INT_TOP_STATUS                     BIT(7)

/* WD_MODE_REG register field definition */
#define TPS65224_BIT_WD_RETURN_LONGWIN			BIT(0)
#define TPS65224_BIT_WD_MODE_SELECT			BIT(1)
#define TPS65224_BIT_WD_PWRHOLD                         BIT(2)
#define TPS65224_BIT_WD_ENDRV_SEL                       BIT(6)
#define TPS65224_BIT_WD_CNT_SEL				BIT(7)

/* WD_QA_CFG register field definition */
#define TPS65224_MASK_WD_QUESTION_SEED			GENMASK(3, 0)
#define TPS65224_MASK_WD_QA_LFSR			GENMASK(5, 4)
#define TPS65224_MASK_WD_QA_FDBK			GENMASK(7, 6)

/* WD_ERR_STATUS register field definition */
#define TPS65224_BIT_WD_LONGWIN_TIMEOUT_INT		BIT(0)
#define TPS65224_BIT_WD_TIMEOUT				BIT(1)
#define TPS65224_BIT_WD_TRIG_EARLY			BIT(2)
#define TPS65224_BIT_WD_ANSW_EARLY			BIT(3)
#define TPS65224_BIT_WD_SEQ_ERR				BIT(4)
#define TPS65224_BIT_WD_ANSW_ERR			BIT(5)
#define TPS65224_BIT_WD_FAIL_INT			BIT(6)
#define TPS65224_BIT_WD_RST_INT				BIT(7)

/* WD_THR_CFG register field definition */
#define TPS65224_MASK_WD_RST_TH				GENMASK(2, 0)
#define TPS65224_MASK_WD_FAIL_TH			GENMASK(5, 3)
#define TPS65224_BIT_WD_EN				BIT(6)
#define TPS65224_BIT_WD_RST_EN				BIT(7)

/* WD_FAIL_CNT_REG register field definition */
#define TPS65224_MASK_WD_FAIL_CNT			GENMASK(3, 0)
#define TPS65224_BIT_WD_FIRST_OK			BIT(5)
#define TPS65224_BIT_WD_BAD_EVENT			BIT(6)

/* CRC8 polynomial for I2C & SPI protocols */
#define TPS65224_CRC8_POLYNOMIAL			0x07

/* IRQs */
enum tps65224_irqs {
	/* INT_BUCK register */
	TPS65224_IRQ_BUCK1_UVOV,
	TPS65224_IRQ_BUCK2_UVOV,
	TPS65224_IRQ_BUCK3_UVOV,
	TPS65224_IRQ_BUCK4_UVOV,
	/* INT_LDO_VMON register */
	TPS65224_IRQ_LDO1_UVOV,
	TPS65224_IRQ_LDO2_UVOV,
	TPS65224_IRQ_LDO3_UVOV,
	TPS65224_IRQ_VCCA_UVOV,
	TPS65224_IRQ_VMON1_UVOV,
	TPS65224_IRQ_VMON2_UVOV,
	/* INT_GPIO register */
	TPS65224_IRQ_GPIO1,
	TPS65224_IRQ_GPIO2,
	TPS65224_IRQ_GPIO3,
	TPS65224_IRQ_GPIO4,
	TPS65224_IRQ_GPIO5,
	TPS65224_IRQ_GPIO6,
	/* INT_STARTUP register */
	TPS65224_IRQ_VSENSE,
	TPS65224_IRQ_ENABLE,
	TPS65224_IRQ_PB_SHORT,
	TPS65224_IRQ_FSD,
	TPS65224_IRQ_SOFT_REBOOT,
	/* INT_MISC register */
	TPS65224_IRQ_BIST_PASS,
	TPS65224_IRQ_EXT_CLK,
	TPS65224_IRQ_REG_UNLOCK,
	TPS65224_IRQ_TWARN,
	TPS65224_IRQ_PB_LONG,
	TPS65224_IRQ_PB_FALL,
	TPS65224_IRQ_PB_RISE,
	TPS65224_IRQ_ADC_CONV_READY,
	/* INT_MODERATE_ERR register */
	TPS65224_IRQ_TSD_ORD,
	TPS65224_IRQ_BIST_FAIL,
	TPS65224_IRQ_REG_CRC_ERR,
	TPS65224_IRQ_RECOV_CNT,
	/* INT_SEVERE_ERR register */
	TPS65224_IRQ_TSD_IMM,
	TPS65224_IRQ_VCCA_OVP,
	TPS65224_IRQ_PFSM_ERR,
	TPS65224_IRQ_BG_XMON,
	/* INT_FSM_ERR register */
	TPS65224_IRQ_IMM_SHUTDOWN,
	TPS65224_IRQ_ORD_SHUTDOWN,
	TPS65224_IRQ_MCU_PWR_ERR,
	TPS65224_IRQ_SOC_PWR_ERR,
	TPS65224_IRQ_COMM_ERR,
	TPS65224_IRQ_I2C2_ERR,
	/* INT_ESM register */
	TPS65224_IRQ_ESM_MCU_PIN,
	TPS65224_IRQ_ESM_MCU_FAIL,
	TPS65224_IRQ_ESM_MCU_RST,
};

#define TPS65224_IRQ_NAME_BUCK1_UVOV		"buck1_uvov"
#define TPS65224_IRQ_NAME_BUCK2_UVOV		"buck2_uvov"
#define TPS65224_IRQ_NAME_BUCK3_UVOV		"buck3_uvov"
#define TPS65224_IRQ_NAME_BUCK4_UVOV		"buck4_uvov"
#define TPS65224_IRQ_NAME_LDO1_UVOV		"ldo1_uvov"
#define TPS65224_IRQ_NAME_LDO2_UVOV		"ldo2_uvov"
#define TPS65224_IRQ_NAME_LDO3_UVOV		"ldo3_uvov"
#define TPS65224_IRQ_NAME_VCCA_UVOV		"vcca_uvov"
#define TPS65224_IRQ_NAME_VMON1_UVOV		"vmon1_uvov"
#define TPS65224_IRQ_NAME_VMON2_UVOV		"vmon2_uvov"
#define TPS65224_IRQ_NAME_GPIO1			"gpio1"
#define TPS65224_IRQ_NAME_GPIO2			"gpio2"
#define TPS65224_IRQ_NAME_GPIO3			"gpio3"
#define TPS65224_IRQ_NAME_GPIO4			"gpio4"
#define TPS65224_IRQ_NAME_GPIO5			"gpio5"
#define TPS65224_IRQ_NAME_GPIO6			"gpio6"
#define TPS65224_IRQ_NAME_VSENSE	        "vsense"
#define TPS65224_IRQ_NAME_ENABLE		"enable"
#define TPS65224_IRQ_NAME_PB_SHORT		"pb_short"
#define TPS65224_IRQ_NAME_FSD			"fsd"
#define TPS65224_IRQ_NAME_SOFT_REBOOT		"soft_reboot"
#define TPS65224_IRQ_NAME_BIST_PASS		"bist_pass"
#define TPS65224_IRQ_NAME_EXT_CLK		"ext_clk"
#define TPS65224_IRQ_NAME_REG_UNLOCK		"reg_unlock"
#define TPS65224_IRQ_NAME_TWARN			"twarn"
#define TPS65224_IRQ_NAME_PB_LONG		"pb_long"
#define TPS65224_IRQ_NAME_PB_FALL		"pb_fall"
#define TPS65224_IRQ_NAME_PB_RISE		"pb_rise"
#define TPS65224_IRQ_NAME_ADC_CONV_READY	"adc_conv_ready"
#define TPS65224_IRQ_NAME_TSD_ORD		"tsd_ord"
#define TPS65224_IRQ_NAME_BIST_FAIL		"bist_fail"
#define TPS65224_IRQ_NAME_REG_CRC_ERR		"reg_crc_err"
#define TPS65224_IRQ_NAME_RECOV_CNT		"recov_cnt"
#define TPS65224_IRQ_NAME_TSD_IMM		"tsd_imm"
#define TPS65224_IRQ_NAME_VCCA_OVP		"vcca_ovp"
#define TPS65224_IRQ_NAME_PFSM_ERR		"pfsm_err"
#define TPS65224_IRQ_NAME_BG_XMON		"bg_xmon"
#define TPS65224_IRQ_NAME_IMM_SHUTDOWN		"imm_shutdown"
#define TPS65224_IRQ_NAME_ORD_SHUTDOWN		"ord_shutdown"
#define TPS65224_IRQ_NAME_MCU_PWR_ERR		"mcu_pwr_err"
#define TPS65224_IRQ_NAME_SOC_PWR_ERR		"soc_pwr_err"
#define TPS65224_IRQ_NAME_COMM_ERR		"comm_err"
#define TPS65224_IRQ_NAME_I2C2_ERR		"i2c2_err"
#define TPS65224_IRQ_NAME_ESM_MCU_PIN		"esm_mcu_pin"
#define TPS65224_IRQ_NAME_ESM_MCU_FAIL		"esm_mcu_fail"
#define TPS65224_IRQ_NAME_ESM_MCU_RST		"esm_mcu_rst"
#define TPS65224_IRQ_NAME_POWERUP		"powerup"

/**
 * struct tps65224 - device private data structure
 *
 * @dev:      MFD parent device
 * @chip_id:  chip ID
 * @reg:      I2C slave address or SPI chip select number
 * @use_crc:  if true, use CRC for I2C and SPI interface protocols
 * @regmap:   regmap for accessing the device registers
 * @irq:      irq generated by the device
 * @irq_data: regmap irq data used for the irq chip
 */
struct tps65224 {
	struct device *dev;
	unsigned long chip_id;
	unsigned short reg;
	bool use_crc;
	struct regmap *regmap;
	int irq;
	struct regmap_irq_chip_data *irq_data;
};

bool tps65224_is_volatile_reg(struct device *dev, unsigned int reg);
int tps65224_device_init(struct tps65224 *tps, bool enable_crc);

#endif /*  __LINUX_MFD_TPS65224_H */
