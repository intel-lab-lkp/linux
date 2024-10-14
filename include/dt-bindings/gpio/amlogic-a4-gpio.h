/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2024 Amlogic, Inc. All rights reserved.
 * Author: Xianwei Zhao <xianwei.zhao@amlogic.com>
 */

#ifndef _DT_BINDINGS_AMLOGIC_A4_GPIO_H
#define _DT_BINDINGS_AMLOGIC_A4_GPIO_H

/* Standard port */
#define GPIOB_START	0
#define GPIOB_NUM	14

#define GPIOD_START	(GPIOB_START + GPIOB_NUM)
#define GPIOD_NUM	16

#define GPIOE_START	(GPIOD_START + GPIOD_NUM)
#define GPIOE_NUM	2

#define GPIOT_START	(GPIOE_START + GPIOE_NUM)
#define GPIOT_NUM	23

#define GPIOX_START	(GPIOT_START + GPIOT_NUM)
#define GPIOX_NUM	18

#define PERIPHS_PIN_NUM	(GPIOX_START + GPIOX_NUM)

/* Aobus port */
#define GPIOAO_START	0
#define GPIOAO_NUM	7

/* It's a special definition, put at the end, just 1 num */
#define	GPIO_TEST_N	(GPIOAO_START +  GPIOAO_NUM)
#define	AOBUS_PIN_NUM	(GPIO_TEST_N + 1)

#define AMLOGIC_GPIO(port, offset)	(port##_START + (offset))

#endif /* _DT_BINDINGS_AMLOGIC_A4_GPIO_H */
