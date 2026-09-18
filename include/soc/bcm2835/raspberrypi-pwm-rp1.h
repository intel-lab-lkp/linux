/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SOC_RASPBERRY_PWM_RP1_H__
#define __SOC_RASPBERRY_PWM_RP1_H__

#include <linux/device.h>

int rp1_pwm_read_tachometer(struct device *dev);

#endif /* __SOC_RASPBERRY_PWM_RP1_H__ */
