/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __IA_CSS_OB_TYPES_H
#define __IA_CSS_OB_TYPES_H

/* @file
* CSS-API header file for Optical Black level parameters.
*/

#include "ia_css_frac.h"

/* Optical black mode.
 */
enum ia_css_ob_mode {
	IA_CSS_OB_MODE_NONE,	/* OB has no effect. */
	IA_CSS_OB_MODE_FIXED,	/* Fixed OB */
	IA_CSS_OB_MODE_RASTER	/* Raster OB */
};

/* Optical Black level configuration.
 *
 *  ISP block: OB1
 *  ISP1: OB1 is used.
 *  ISP2: OB1 is used.
 */
struct ia_css_ob_config {
	/*
	 * Mode (None / Fixed / Raster).
	 * enum, [0,2],
	 * default 1, ineffective 0
	 */
	enum ia_css_ob_mode mode;
	/*
	 * Black level for GR pixels
	 * (used for Fixed Mode only).
	 * u0.16, [0,65535],
	 * default/ineffective 0
	 */
	ia_css_u0_16 level_gr;
	/*
	 * Black level for R pixels
	 * (used for Fixed Mode only).
	 * u0.16, [0,65535],
	 * default/ineffective 0
	 */
	ia_css_u0_16 level_r;
	/*
	 * Black level for B pixels
	 * (used for Fixed Mode only).
	 * u0.16, [0,65535],
	 * default/ineffective 0
	 */
	ia_css_u0_16 level_b;
	/*
	 * Black level for GB pixels
	 * (used for Fixed Mode only).
	 * u0.16, [0,65535],
	 * default/ineffective 0
	 */
	ia_css_u0_16 level_gb;
	/*
	 * Start position of OB area
	 * (used for Raster Mode only).
	 * u16.0, [0,63],
	 * default/ineffective 0
	 */
	u16 start_position;
	/*
	 * End position of OB area
	 * (used for Raster Mode only).
	 * u16.0, [0,63],
	 * default/ineffective 0
	 */
	u16 end_position;
};

#endif /* __IA_CSS_OB_TYPES_H */
