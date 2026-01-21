/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Junhui Liu <junhui.liu@pigmoral.tech>
 */

#ifndef _CCU_SUN60I_A733_RTC_H_
#define _CCU_SUN60I_A733_RTC_H_

#include <dt-bindings/clock/sun60i-a733-rtc.h>

#define CLK_IOSC_32K		9
#define CLK_EXT_OSC32K_GATE	10
#define CLK_HOSC_32K_MUX	11
#define CLK_HOSC_32K		12

#define CLK_NUMBER		(CLK_HOSC_32K + 1)

#endif /* _CCU_SUN60I_A733_RTC_H_ */
