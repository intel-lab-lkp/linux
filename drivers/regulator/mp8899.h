/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __MP8899_H__
#define __MP8899_H__

/* Buck Control Registers */
#define MP8899_BUCK1_CTL1		0x00
#define MP8899_BUCK1_CTL2		0x01
#define MP8899_BUCK1_CTL3		0x02
#define MP8899_BUCK1_CTL4		0x03
#define MP8899_BUCK1_CTL5		0x04
#define MP8899_BUCK1_CTL6		0x05

#define MP8899_BUCK2_CTL1		0x06
#define MP8899_BUCK2_CTL2		0x07
#define MP8899_BUCK2_CTL3		0x08
#define MP8899_BUCK2_CTL4		0x09
#define MP8899_BUCK2_CTL5		0x0A
#define MP8899_BUCK2_CTL6		0x0B

#define MP8899_BUCK3_CTL1		0x0C
#define MP8899_BUCK3_CTL2		0x0D
#define MP8899_BUCK3_CTL3		0x0E
#define MP8899_BUCK3_CTL4		0x0F
#define MP8899_BUCK3_CTL5		0x10
#define MP8899_BUCK3_CTL6		0x11

#define MP8899_BUCK4_CTL1		0x12
#define MP8899_BUCK4_CTL2		0x13
#define MP8899_BUCK4_CTL3		0x14
#define MP8899_BUCK4_CTL4		0x15
#define MP8899_BUCK4_CTL5		0x16
#define MP8899_BUCK4_CTL6		0x17

/* System Control Registers */
#define MP8899_SYSTEM1			0x18
#define MP8899_SYSTEM2			0x19
#define MP8899_SYSTEM3			0x1A
#define MP8899_STATUS			0x1F
#define MP8899_SYSTEM4			0x21

/* BUCK_CTL1 Register Bit Definitions */
#define MP8899_VOUT_DIS_EN_MASK		BIT(7)
#define MP8899_PHASE_DELAY_MASK		GENMASK(6, 5)
#define MP8899_PHASE_DELAY_SHIFT	5
#define MP8899_MODE_MASK		GENMASK(4, 3)
#define MP8899_VOUT_OVP_EN_MASK		BIT(2)

/* BUCK_CTL2 Register Bit Definitions */
#define MP8899_SOFT_STOP_EN_MASK	BIT(7)
#define MP8899_SOFT_START_TIME_MASK	GENMASK(3, 2)
#define MP8899_SOFT_START_TIME_SHIFT	2
#define MP8899_SOFT_STOP_TIME_MASK	GENMASK(1, 0)

/* BUCK_CTL3 Register Bit Definitions */
#define MP8899_VOUT_SELECT_MASK		BIT(6)
#define MP8899_CURRENT_LIMIT_MASK	GENMASK(5, 4)
#define MP8899_ADDITIONAL_PHASE_DELAY_MASK GENMASK(3, 2)
#define MP8899_TIME_SLOT_MASK		GENMASK(1, 0)

/* BUCK_CTL4 Register Bit Definitions */
#define MP8899_VREF_HIGH_MASK		GENMASK(3, 0)

/* BUCK_CTL6 Register Bit Definitions */
#define MP8899_GO_BIT_MASK		BIT(7)

/* SYSTEM1 Register Bit Definitions */
#define MP8899_EN_BASE			7
#define MP8899_EN1_MASK			BIT(7)
#define MP8899_EN2_MASK			BIT(6)
#define MP8899_EN3_MASK			BIT(5)
#define MP8899_EN4_MASK			BIT(4)
#define MP8899_TIME_SLOT_DURATION_MASK	GENMASK(3, 2)
#define MP8899_SHUTDOWN_DELAY_EN_MASK	BIT(1)
#define MP8899_OP_BIT_MASK		BIT(0)

/* SYSTEM2 Register Bit Definitions */
#define MP8899_FREQ_MASK		GENMASK(7, 5)
#define MP8899_I2C_ADDR_MASK		GENMASK(4, 0)

/* SYSTEM3 Register Bit Definitions */
#define MP8899_ADD_PG_OP_MASK		GENMASK(7, 6)
#define MP8899_GPIO_POWER_GOOD		0x0
#define MP8899_GPIO_ADDRESS		0x1
#define MP8899_GPIO_OUTPUT_PORT		0x2
#define MP8899_PROTECT_MODE_MASK	BIT(5)
#define MP8899_PG_DELAY_MASK		GENMASK(3, 2)

/* SYSTEM4 Register Bit Definitions */
#define MP8899_VENDOR_ID_MASK		GENMASK(7, 4)
#define MP8899_VENDOR_ID_VALUE		0x8

/* STATUS Register Bit Definitions */
#define MP8899_PG1_MASK			BIT(7)
#define MP8899_PG2_MASK			BIT(6)
#define MP8899_PG3_MASK			BIT(5)
#define MP8899_PG4_MASK			BIT(4)
#define MP8899_OT_WARNING_MASK		BIT(3)
#define MP8899_OT_PROTECTION_MASK	BIT(2)

/* Voltage Reference Definitions */
#define MP8899_VREF_MIN_UV		400000
#define MP8899_VREF_RANGE1_MAX_UV	2047500
#define MP8899_VREF_RANGE2_MAX_UV	3600000
#define MP8899_VREF_STEP1_UV		500
#define MP8899_VREF_STEP2_UV		1000

/* Timeout and Bit Position Definitions */
#define MP8899_GO_BIT_TIMEOUT_MS	10
#define MP8899_GO_BIT_POLL_INTERVAL_US	10
#define MP8899_PG_BIT_BASE		7

/* For 0.5mV step (VOUT_SELECT=0): (2.0475V-0.4V)/0.5mV + 1 */
/* For 1mV step (VOUT_SELECT=1): (3600V-400V)/1mV+1 = 3201 */
#define MP8899_N_VOLTAGES		3296
#define MP8899_N_VOLTAGES_1MV		3201

/* Current Limit Definitions */
#define MP8899_CURRENT_LIMIT_2A		0
#define MP8899_CURRENT_LIMIT_3A		1
#define MP8899_CURRENT_LIMIT_4_2A	2
#define MP8899_CURRENT_LIMIT_5A		3

/* Phase Delay Definitions */
#define MP8899_PHASE_DELAY_0		0
#define MP8899_PHASE_DELAY_90		1
#define MP8899_PHASE_DELAY_180		2
#define MP8899_PHASE_DELAY_270		3

/* Switching Frequency Definitions */
#define MP8899_FREQ_400KHZ		0
#define MP8899_FREQ_650KHZ		1
#define MP8899_FREQ_800KHZ		2
#define MP8899_FREQ_1000KHZ		3
#define MP8899_FREQ_1200KHZ		4
#define MP8899_FREQ_1400KHZ		5
#define MP8899_FREQ_2000KHZ		6
#define MP8899_FREQ_3000KHZ		7

/* Register offset calculations */
#define MP8899_BUCK_CTL_OFFSET		6

/* Helper macros for register access */
#define MP8899_BUCK_REG(buck, reg)	((reg) + ((buck) * MP8899_BUCK_CTL_OFFSET))

struct mp8899_regulator_info;

#endif /* __MP8899_H__ */
