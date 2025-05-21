/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Renesas RZ System Controller
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#ifndef __SOC_RENESAS_RZ_SYSC_H__
#define __SOC_RENESAS_RZ_SYSC_H__

#include <linux/device.h>
#include <linux/refcount.h>
#include <linux/regmap.h>
#include <linux/sys_soc.h>
#include <linux/types.h>

/**
 * struct rz_sysc_signal_init_data - RZ SYSC signals init data
 * @name: signal name
 * @offset: register offset controling this signal
 * @mask: bitmask in register specific to this signal
 * @refcnt_incr_val: increment refcnt when setting this value
 */
struct rz_sysc_signal_init_data {
	const char *name;
	u32 offset;
	u32 mask;
	u32 refcnt_incr_val;
};

/**
 * struct rz_sysc_signal - RZ SYSC signals
 * @init_data: signals initialization data
 * @refcnt: reference counter
 */
struct rz_sysc_signal {
	struct rz_sysc_signal_init_data init_data;
	refcount_t refcnt;
};

/**
 * struct rz_syc_soc_id_init_data - RZ SYSC SoC identification initialization data
 * @family: RZ SoC family
 * @id: RZ SoC expected ID
 * @devid_offset: SYSC SoC ID register offset
 * @revision_mask: SYSC SoC ID revision mask
 * @specific_id_mask: SYSC SoC ID specific ID mask
 * @print_id: print SoC-specific extended device identification
 */
struct rz_sysc_soc_id_init_data {
	const char * const family;
	u32 id;
	u32 devid_offset;
	u32 revision_mask;
	u32 specific_id_mask;
	void (*print_id)(struct device *dev, void __iomem *sysc_base,
			 struct soc_device_attribute *soc_dev_attr);
};

/**
 * struct rz_sysc_init_data - RZ SYSC initialization data
 * @soc_id_init_data: RZ SYSC SoC ID initialization data
 * @signals_init_data: RZ SYSC signals initialization data
 * @regmap_cfg: SoC-specific regmap config
 * @num_signals: number of SYSC signals
 */
struct rz_sysc_init_data {
	const struct rz_sysc_soc_id_init_data *soc_id_init_data;
	const struct rz_sysc_signal_init_data *signals_init_data;
	const struct regmap_config *regmap_cfg;
	u32 max_register_offset;
	u32 num_signals;
};

extern int rz_sysc_reg_read(void *context, unsigned int off, unsigned int *val);
extern int rz_sysc_reg_write(void *context, unsigned int off, unsigned int val);
extern int rz_sysc_reg_update_bits(void *context, unsigned int off,
				   unsigned int mask, unsigned int val);

extern const struct rz_sysc_init_data rzg3e_sys_init_data;
extern const struct rz_sysc_init_data rzg3s_sysc_init_data;
extern const struct rz_sysc_init_data rzv2h_sys_init_data;
extern const struct rz_sysc_init_data rzv2n_sys_init_data;

#endif /* __SOC_RENESAS_RZ_SYSC_H__ */
