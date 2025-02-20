/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LP5812 Register Header
 *
 * Copyright (C) 2025 Texas Instruments
 *
 * Author: Jared Zhou <jared-zhou@ti.com>
 */

#ifndef _LEDS_LP5812_REGS_H_
#define _LEDS_LP5812_REGS_H_

#define CHIP_EN_REG                     0x00

#define DEV_CONFIG0                     0x01
#define DEV_CONFIG1                     0x02
#define DEV_CONFIG2                     0x03
#define DEV_CONFIG3                     0x04
#define DEV_CONFIG4                     0x05
#define DEV_CONFIG5                     0x06
#define DEV_CONFIG6                     0x07
#define DEV_CONFIG7                     0x08
#define DEV_CONFIG8                     0x09
#define DEV_CONFIG9                     0x0A
#define DEV_CONFIG10                    0x0B
#define DEV_CONFIG11                    0x0c
#define DEV_CONFIG12                    0x0D

#define CMD_UPDATE_REG                  0x10
#define CMD_START_REG                   0x11
#define CMD_STOP_REG                    0x12
#define CMD_PAUSE_REG                   0x13
#define CMD_CONTINUE_REG                0x14

#define LED_ENABLE_1_REG                0x20
#define LED_ENABLE_2_REG                0x21

#define FAULT_CLEAR_REG                 0x22
#define RESET_REG                       0x23

#define MANUAL_DC_LED_0_REG             0x30
#define MANUAL_PWM_LED_0_REG            0x40
#define AUTO_DC_LED_0_REG               0x50

/* value for register */
#define UPDATE_CMD_VAL                  0x55
#define START_CMD_VAL                   0xFF
#define STOP_CMD_VAL                    0xAA
#define PAUSE_CMD_VAL                   0x33
#define CONTINUE_CMD_VAL                0xCC

#define CHIP_ENABLE                     0x01
#define CHIP_DISABLE                    0x00

#define FAULT_CLEAR_ALL                 0x07
#define TSD_CLEAR_VAL                   0x04
#define LSD_CLEAR_VAL                   0x02
#define LOD_CLEAR_VAL                   0x01
#define RESET_REG_VAL                   0x66

#define LED0_AUTO_BASE_ADRR             0x80
#define LED1_AUTO_BASE_ADRR             0x9A
#define LED2_AUTO_BASE_ADRR             0xB4
#define LED3_AUTO_BASE_ADRR             0xCE
#define LED_A0_AUTO_BASE_ADRR           0xE8
#define LED_A1_AUTO_BASE_ADRR           0x102
#define LED_A2_AUTO_BASE_ADRR           0x11C
#define LED_B0_AUTO_BASE_ADRR           0x136
#define LED_B1_AUTO_BASE_ADRR           0x150
#define LED_B2_AUTO_BASE_ADRR           0x16A
#define LED_C0_AUTO_BASE_ADRR           0x184
#define LED_C1_AUTO_BASE_ADRR           0x19E
#define LED_C2_AUTO_BASE_ADRR           0x1B8
#define LED_D0_AUTO_BASE_ADRR           0x1D2
#define LED_D1_AUTO_BASE_ADRR           0x1EC
#define LED_D2_AUTO_BASE_ADRR           0x206

/* Flag Registers */
#define TSD_CONFIG_STAT_REG             0x300
#define LOD_STAT_1_REG                  0x301
#define LOD_STAT_2_REG                  0x302
#define LSD_STAT_1_REG                  0x303
#define LSD_STAT_2_REG                  0x304

#define AUTO_PWM_BASE_ADDR              0x305

#define AEP_STATUS_0_REG                0x315
#define AEP_STATUS_1_REG                0x316
#define AEP_STATUS_2_REG                0x317
#define AEP_STATUS_3_REG                0x318
#define AEP_STATUS_4_REG                0x319
#define AEP_STATUS_5_REG                0x31A
#define AEP_STATUS_6_REG                0x31B
#define AEP_STATUS_7_REG                0x31C

#endif /*_LEDS_LP5812_REGS_H_ */
