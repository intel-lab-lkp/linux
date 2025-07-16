/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2025 Hisilicon Limited. */

#ifndef __HBG_LED_H
#define __HBG_LED_H

#include "hbg_common.h"

struct hbg_led_classdev {
	struct hbg_priv *priv;
	struct led_classdev led;
	u32 index;
};

int hbg_leds_init(struct hbg_priv *priv);

#endif
