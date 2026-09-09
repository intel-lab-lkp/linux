/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Renesas RZ System Controller
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#ifndef __SOC_RENESAS_RZ_SYSC_H__
#define __SOC_RENESAS_RZ_SYSC_H__

#include <linux/device.h>
#include <linux/sys_soc.h>
#include <linux/types.h>

/**
 * struct rz_aof_unit - RZ AOF unit description
 * @base: MMIO base of the master IP (low 32 bits; all < 4G)
 * @index: index of SYS_AOFn (0-based)
 * @addr_bits: address width of the master IP (35 or 36 bits)
 */
struct rz_aof_unit {
	u32 base;
	u8 index;
	u8 addr_bits;
};

/**
 * struct rz_syc_soc_id_init_data - RZ SYSC SoC identification initialization data
 * @family: RZ SoC family
 * @id: RZ SoC expected ID
 * @devid_offset: SYSC SoC ID register offset
 * @revision_mask: SYSC SoC ID revision mask
 * @specific_id_mask: SYSC SoC ID specific ID mask
 * @print_id: print SoC-specific extended device identification
 * @pwrrdy_pwrseq: has pwrrdy register controlled through power sequencer
 * @aof_units: array of AOF units for this SoC, or NULL if it has none
 * @num_aof_units: number of entries in @aof_units
 * @aof_base: offset of SYS_AOF0 from the SYSC base; ignored if @aof_units is NULL
 *            (this offset is SoC-specific, unlike the fixed inter-register stride)
 */
struct rz_sysc_soc_id_init_data {
	const char * const family;
	u32 id;
	u32 devid_offset;
	u32 revision_mask;
	u32 specific_id_mask;
	void (*print_id)(struct device *dev, void __iomem *sysc_base,
			 struct soc_device_attribute *soc_dev_attr);
	bool pwrrdy_pwrseq;
	const struct rz_aof_unit *aof_units;
	unsigned int num_aof_units;
	u32 aof_base;
};

#define SYS_AOF_STRIDE			0x0004
#define SYS_AOF_OFFSET(aof_base, n)	((aof_base) + ((n) * SYS_AOF_STRIDE))

/**
 * struct rz_sysc_init_data - RZ SYSC initialization data
 * @soc_id_init_data: RZ SYSC SoC ID initialization data
 * @writeable_reg: Regmap writeable register check function
 * @readable_reg: Regmap readable register check function
 * @max_register: Maximum SYSC register offset to be used by the regmap config
 */
struct rz_sysc_init_data {
	const struct rz_sysc_soc_id_init_data *soc_id_init_data;
	bool (*writeable_reg)(struct device *dev, unsigned int reg);
	bool (*readable_reg)(struct device *dev, unsigned int reg);
	u32 max_register;
};

extern const struct rz_sysc_init_data rzg3e_sys_init_data;
extern const struct rz_sysc_init_data rzg3l_sysc_init_data;
extern const struct rz_sysc_init_data rzg3s_sysc_init_data;
extern const struct rz_sysc_init_data rzv2h_sys_init_data;
extern const struct rz_sysc_init_data rzv2n_sys_init_data;

#endif /* __SOC_RENESAS_RZ_SYSC_H__ */
