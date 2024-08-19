/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef __HBG_HW_H
#define __HBG_HW_H

#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/regmap.h>

#define hbg_reg_read(priv, reg_addr) ({ \
	u32 _value = U32_MAX; \
	(void)regmap_read((priv)->regmap, reg_addr, &_value); \
	_value; })

#define hbg_reg_write(priv, reg_addr, value) \
	regmap_write((priv)->regmap, reg_addr, value)

#define hbg_reg_read64(priv, reg_addr) lo_hi_readq((priv)->io_base + (reg_addr))

#define hbg_reg_write64(priv, reg_addr, value) \
	lo_hi_writeq(value, (priv)->io_base + (reg_addr))

#define hbg_reg_read_field(priv, reg_addr, mask) \
	FIELD_GET(mask, hbg_reg_read(priv, reg_addr))

#define hbg_reg_write_field(priv, reg_addr, mask, val) \
	regmap_write_bits((priv)->regmap, reg_addr, mask, FIELD_PREP(mask, val))

int hbg_hw_init(struct hbg_priv *pri);
void hbg_hw_adjust_link(struct hbg_priv *priv, u32 speed, u32 duplex);

#endif
