/* SPDX-License-Identifier: GPL-2.0 */

/*
 *Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

/*
 * Timer Register Offset Definitions of Timer 1, Increment base address by 4
 * and use same offsets for Timer 2
 */
#define TTC_CLK_CNTRL_OFFSET            0x00 /* Clock Control Reg, RW */
#define TTC_CNT_CNTRL_OFFSET            0x0C /* Counter Control Reg, RW */
#define TTC_COUNT_VAL_OFFSET            0x18 /* Counter Value Reg, RO */
#define TTC_INTR_VAL_OFFSET             0x24 /* Interval Count Reg, RW */
#define TTC_MATCH_CNT_VAL_OFFSET        0x30 /* Match Count Reg, RW */
#define TTC_ISR_OFFSET          0x54 /* Interrupt Status Reg, RO */
#define TTC_IER_OFFSET          0x60 /* Interrupt Enable Reg, RW */

#define TTC_CNT_CNTRL_DISABLE_MASK      0x1

#define TTC_CLK_CNTRL_CSRC_MASK         (1 << 5)        /* clock source */
#define TTC_CLK_CNTRL_PSV_MASK		0x1e
#define TTC_CLK_CNTRL_PSV_SHIFT         1

/*
 * Setup the timers to use pre-scaling, using a fixed value for now that will
 * work across most input frequency, but it may need to be more dynamic
 */
#define PRESCALE_EXPONENT       11      /* 2 ^ PRESCALE_EXPONENT = PRESCALE */
#define PRESCALE                2048    /* The exponent must match this */
#define CLK_CNTRL_PRESCALE      ((PRESCALE_EXPONENT - 1) << 1)
#define CLK_CNTRL_PRESCALE_EN   1
#define CNT_CNTRL_RESET         (1 << 4)

#define MAX_F_ERR 50

#define TTC_PWM_CHANNEL         0x4

#define TTC_CLK_CNTRL_CSRC              BIT(5)
#define TTC_CLK_CNTRL_PS_EN             BIT(0)
#define TTC_CNTR_CTRL_DIS               BIT(0)
#define TTC_CNTR_CTRL_INTR_MODE_EN      BIT(1)
#define TTC_CNTR_CTRL_MATCH_MODE_EN     BIT(3)
#define TTC_CNTR_CTRL_RST               BIT(4)
#define TTC_CNTR_CTRL_WAVE_EN   BIT(5)
#define TTC_CNTR_CTRL_WAVE_POL  BIT(6)
#define TTC_CNTR_CTRL_WAVE_POL_SHIFT    6
#define TTC_CNTR_CTRL_PRESCALE_SHIFT    1
#define TTC_PWM_MAX_CH	3

#if defined(CONFIG_PWM_CADENCE)
int ttc_pwm_probe(struct platform_device *pdev);
void ttc_pwm_remove(struct platform_device *pdev);
#endif

