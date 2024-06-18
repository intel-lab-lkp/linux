/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * max77705-private.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_MFD_MAX77705_PRIV_H
#define __LINUX_MFD_MAX77705_PRIV_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/i2c.h>

#define MAX77705_REG_INVALID		(0xff)
#define MAX77705_IRQSRC_CHG			BIT(0)
#define MAX77705_IRQSRC_TOP			BIT(1)
#define MAX77705_IRQSRC_FG			BIT(2)
#define MAX77705_IRQSRC_USBC		BIT(3)

/* STATUS_REG */
#define MAX77705_BAT_ABSENT_MASK	BIT(3)
/* MAX77705 MAINCTRL1 register */
#define MAX77705_MAINCTRL1_BIASEN_SHIFT		7
#define MAX77705_MAINCTRL1_BIASEN_MASK		BIT(MAX77705_MAINCTRL1_BIASEN_SHIFT)

/* max77705-haptic configuration register */
#define MAX77705_CONFIG2_MEN_SHIFT		6
#define MAX77705_CONFIG2_MODE_SHIFT		7
#define MAX77705_CONFIG2_HTYP_SHIFT		5

#define MAX77705_REVISION_MASK	7
#define MAX77705_VERSION_MASK	0xF8
#define MAX77705_VERSION_SHIFT	3

enum max77705_hw_rev {
	MAX77705_PASS1 = 1,
	MAX77705_PASS2,
	MAX77705_PASS3,
};

enum max77705_reg {
	/* Slave addr = 0xCC */
	/* PMIC Top-Level Registers */
	MAX77705_PMIC_REG_PMICID1		= 0x00,
	MAX77705_PMIC_REG_PMICREV		= 0x01,
	MAX77705_PMIC_REG_MAINCTRL1		= 0x02,
	MAX77705_PMIC_REG_INTSRC		= 0x22,
	MAX77705_PMIC_REG_INTSRC_MASK		= 0x23,
	MAX77705_PMIC_REG_SYSTEM_INT		= 0x24,
	MAX77705_PMIC_REG_RESERVED_25		= 0x25,
	MAX77705_PMIC_REG_SYSTEM_INT_MASK	= 0x26,
	MAX77705_PMIC_REG_RESERVED_27		= 0x27,
	MAX77705_PMIC_REG_RESERVED_28		= 0x28,
	MAX77705_PMIC_REG_RESERVED_29		= 0x29,
	MAX77705_PMIC_REG_BOOSTCONTROL1		= 0x4C,
	MAX77705_PMIC_REG_BSTOUT_MASK		= 0x03,
	MAX77705_PMIC_REG_BOOSTCONTROL2		= 0x4F,
	MAX77705_PMIC_REG_FORCE_EN_MASK		= 0x08,
	MAX77705_PMIC_REG_SW_RESET		= 0x50,
	MAX77705_PMIC_REG_USBC_RESET		= 0x51,

	/* Haptic motor driver Registers */
	MAX77705_PMIC_REG_MCONFIG		= 0x10,
	MAX77705_PMIC_REG_MCONFIG2		= 0x11,

	MAX77705_CHG_REG_INT			= 0xB0,
	MAX77705_CHG_REG_INT_MASK		= 0xB1,
	MAX77705_CHG_REG_INT_OK			= 0xB2,
	MAX77705_CHG_REG_DETAILS_00		= 0xB3,
	MAX77705_CHG_REG_DETAILS_01		= 0xB4,
	MAX77705_CHG_REG_DETAILS_02		= 0xB5,
	MAX77705_CHG_REG_DTLS_03		= 0xB6,
	MAX77705_CHG_REG_CNFG_00		= 0xB7,
	MAX77705_CHG_REG_CNFG_01		= 0xB8,
	MAX77705_CHG_REG_CNFG_02		= 0xB9,
	MAX77705_CHG_REG_CNFG_03		= 0xBA,
	MAX77705_CHG_REG_CNFG_04		= 0xBB,
	MAX77705_CHG_REG_CNFG_05		= 0xBC,
	MAX77705_CHG_REG_CNFG_06		= 0xBD,
	MAX77705_CHG_REG_CNFG_07		= 0xBE,
	MAX77705_CHG_REG_CNFG_08		= 0xBF,
	MAX77705_CHG_REG_CNFG_09		= 0xC0,
	MAX77705_CHG_REG_CNFG_10		= 0xC1,
	MAX77705_CHG_REG_CNFG_11		= 0xC2,
	MAX77705_CHG_REG_CNFG_12		= 0xC3,
	MAX77705_CHG_REG_CNFG_13		= 0xC4,
	MAX77705_CHG_REG_CNFG_14		= 0xC5,
	MAX77705_CHG_REG_SAFEOUT_CTRL		= 0xC6,

	MAX77705_PMIC_REG_END,
};

enum max77705_charger_battery_state {
	MAX77705_BATTERY_NOBAT,
	MAX77705_BATTERY_PREQUALIFICATION,
	MAX77705_BATTERY_DEAD,
	MAX77705_BATTERY_GOOD,
	MAX77705_BATTERY_LOWVOLTAGE,
	MAX77705_BATTERY_OVERVOLTAGE,
	MAX77705_BATTERY_RESERVED,
};

enum max77705_charger_charge_type {
	MAX77705_CHARGER_CONSTANT_CURRENT	= 1,
	MAX77705_CHARGER_CONSTANT_VOLTAGE,
	MAX77705_CHARGER_END_OF_CHARGE,
	MAX77705_CHARGER_DONE,
};

/* Slave addr = 0x6C : Fuelgauge */
enum max77705_fuelgauge_reg {
	STATUS_REG				= 0x00,
	VALRT_THRESHOLD_REG			= 0x01,
	TALRT_THRESHOLD_REG			= 0x02,
	SALRT_THRESHOLD_REG			= 0x03,
	REMCAP_REP_REG				= 0x05,
	SOCREP_REG				= 0x06,
	TEMPERATURE_REG				= 0x08,
	VCELL_REG				= 0x09,
	TIME_TO_EMPTY_REG			= 0x11,
	FULLSOCTHR_REG				= 0x13,
	CURRENT_REG				= 0x0A,
	AVG_CURRENT_REG				= 0x0B,
	SOCMIX_REG				= 0x0D,
	SOCAV_REG				= 0x0E,
	REMCAP_MIX_REG				= 0x0F,
	FULLCAP_REG				= 0x10,
	RFAST_REG				= 0x15,
	AVR_TEMPERATURE_REG			= 0x16,
	CYCLES_REG				= 0x17,
	DESIGNCAP_REG				= 0x18,
	AVR_VCELL_REG				= 0x19,
	TIME_TO_FULL_REG			= 0x20,
	CONFIG_REG				= 0x1D,
	ICHGTERM_REG				= 0x1E,
	REMCAP_AV_REG				= 0x1F,
	FULLCAP_NOM_REG				= 0x23,
	LEARN_CFG_REG				= 0x28,
	FILTER_CFG_REG				= 0x29,
	MISCCFG_REG				= 0x2B,
	QRTABLE20_REG				= 0x32,
	FULLCAP_REP_REG				= 0x35,
	RCOMP_REG				= 0x38,
	VEMPTY_REG				= 0x3A,
	FSTAT_REG				= 0x3D,
	DISCHARGE_THRESHOLD_REG			= 0x40,
	QRTABLE30_REG				= 0x42,
	ISYS_REG				= 0x43,
	DQACC_REG				= 0x45,
	DPACC_REG				= 0x46,
	AVGISYS_REG				= 0x4B,
	QH_REG					= 0x4D,
	VSYS_REG				= 0xB1,
	TALRTTH2_REG				= 0xB2,
	/* "not used REG(0xB2)" is for checking fuelgague init result. */
	FG_INIT_RESULT_REG			= TALRTTH2_REG,
	VBYP_REG				= 0xB3,
	CONFIG2_REG				= 0xBB,
	IIN_REG					= 0xD0,
	OCV_REG					= 0xEE,
	VFOCV_REG				= 0xFB,
	VFSOC_REG				= 0xFF,

	MAX77705_FG_END,
};

enum max77705_irq_source {
	SYS_INT = 0,
	CHG_INT,
	FUEL_INT,
	MAX77705_IRQ_GROUP_NR,
};


#define MAX77705_REG_MAINCTRL1_BIASEN		BIT(7)

/* Slave addr = 0x94: RGB LED */
enum max77705_led_reg {
	MAX77705_RGBLED_REG_LEDEN			= 0x30,
	MAX77705_RGBLED_REG_LED0BRT			= 0x31,
	MAX77705_RGBLED_REG_LED1BRT			= 0x32,
	MAX77705_RGBLED_REG_LED2BRT			= 0x33,
	MAX77705_RGBLED_REG_LED3BRT			= 0x34,
	MAX77705_RGBLED_REG_LEDRMP			= 0x36,
	MAX77705_RGBLED_REG_LEDBLNK			= 0x38,
	MAX77705_LED_REG_END,
};

enum max77705_irq {
	/* PMIC; TOPSYS */
	MAX77705_SYSTEM_IRQ_BSTEN_INT,
	MAX77705_SYSTEM_IRQ_SYSUVLO_INT,
	MAX77705_SYSTEM_IRQ_SYSOVLO_INT,
	MAX77705_SYSTEM_IRQ_TSHDN_INT,
	MAX77705_SYSTEM_IRQ_TM_INT,

	/* PMIC; Charger */
	MAX77705_CHG_IRQ_BYP_I,
	MAX77705_CHG_IRQ_BAT_I,
	MAX77705_CHG_IRQ_CHG_I,
	MAX77705_CHG_IRQ_WCIN_I,
	MAX77705_CHG_IRQ_CHGIN_I,
	MAX77705_CHG_IRQ_AICL_I,

	/* Fuelgauge */
	MAX77705_FG_IRQ_ALERT,

	MAX77705_IRQ_NR,
};

struct max77705_dev {
	struct device *dev;
	struct i2c_client *i2c; /* 0xCC; Haptic, PMIC */
	struct i2c_client *charger; /* 0xD2; Charger */
	struct i2c_client *fuelgauge; /* 0x6C; Fuelgauge */
	struct i2c_client *muic; /* 0x4A; MUIC */
	struct i2c_client *debug; /* 0xC4; Debug */
	struct mutex i2c_lock;

	struct regmap *regmap;
	struct regmap *regmap_fg;
	struct regmap *regmap_charger;
	struct regmap *regmap_leds;

	int type;

	int irq;
	int irq_base;
	int irq_masks_cur[MAX77705_IRQ_GROUP_NR];
	int irq_masks_cache[MAX77705_IRQ_GROUP_NR];
	bool wakeup;
	struct mutex irqlock;

#ifdef CONFIG_HIBERNATION
	/* For hibernation */
	u8 reg_pmic_dump[MAX77705_PMIC_REG_END];
	u8 reg_muic_dump[MAX77705_USBC_REG_END];
	u8 reg_led_dump[MAX77705_LED_REG_END];
#endif

	/* pmic VER/REV register */
	u8 pmic_rev;	/* pmic Rev */
	u8 pmic_ver;	/* pmic version */

	u8 cc_booting_complete;

	wait_queue_head_t queue_empty_wait_q;
	int doing_irq;
	int is_usbc_queue;

	struct max77705_platform_data *pdata;
};

enum max77705_types {
	TYPE_MAX77705,
};


/**
 * Unmask sub device interrupts on device level
 *
 * @param max77705 - device structure
 * @param mask - sub device interrupts to unmask
 */
extern inline int max77705_irq_unmask_subdevice(struct max77705_dev *max77705, unsigned int mask);

/**
 * Same as max77705_irq_unmask_device, but for masking.
 */
extern inline int max77705_irq_mask_subdevice(struct max77705_dev *max77705, unsigned int mask);


extern int max77705_irq_init(struct max77705_dev *max77705);

#endif /* __LINUX_MFD_MAX77705_PRIV_H */
