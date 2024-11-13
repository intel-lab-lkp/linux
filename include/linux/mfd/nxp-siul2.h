/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * S32 SIUL2 core definitions
 *
 * Copyright 2024 NXP
 */

#ifndef __DRIVERS_MFD_NXP_SIUL2_H
#define __DRIVERS_MFD_NXP_SIUL2_H

#include <linux/regmap.h>

/**
 * enum nxp_siul2_reg_type - an enum for SIUL2 reg types
 * @SIUL2_MPIDR - SoC info
 * @SIUL2_IRQ - IRQ related registers, only valid in SIUL2_1
 * @SIUL2_MSCR - used for pinmuxing and pinconf
 * @SIUL2_IMCR - used for pinmuxing
 * @SIUL2_PGPDO - writing the GPIO value
 * @SIUL2_PGPDI - reading the GPIO value
 */
enum nxp_siul2_reg_type {
	SIUL2_MPIDR,
	SIUL2_IRQ,
	SIUL2_MSCR,
	SIUL2_IMCR,
	SIUL2_PGPDO,
	SIUL2_PGPDI,

	SIUL2_NUM_REG_TYPES
};

/**
 * struct nxp_siul2_info - details about one SIUL2 hardware instance
 * @regmaps: the regmaps for each register type for a SIUL2 hardware instance
 * @gpio_base: the first GPIO in this SIUL2 module
 * @gpio_num: the number of GPIOs in this SIUL2 module
 */
struct nxp_siul2_info {
	struct regmap *regmaps[SIUL2_NUM_REG_TYPES];
	u32 gpio_base;
	u32 gpio_num;
};

/**
 * struct nxp_siul2_mfd - driver data
 * @siul2: info about the SIUL2 modules present
 * @num_siul2: number of siul2 modules
 */
struct nxp_siul2_mfd {
	struct nxp_siul2_info *siul2;
	u8 num_siul2;
};

#endif /* __DRIVERS_MFD_NXP_SIUL2_H */
