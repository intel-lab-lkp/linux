/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef __HBG_IRQ_H
#define __HBG_IRQ_H

#include "hbg_common.h"

void hbg_irq_enable(struct hbg_priv *priv, u32 mask, bool enable);
bool hbg_irq_is_enabled(struct hbg_priv *priv, u32 mask);
int hbg_irq_init(struct hbg_priv *priv);

#endif
