/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __IA_CSS_FPN_TYPES_H
#define __IA_CSS_FPN_TYPES_H

/* @file
* CSS-API header file for Fixed Pattern Noise parameters.
*/

/* Fixed Pattern Noise table.
 *
 *  This contains the fixed patterns noise values
 *  obtained from a black frame capture.
 *
 *  "shift" should be set as the smallest value
 *  which satisfies the requirement the maximum data is less than 64.
 *
 *  ISP block: FPN1
 *  ISP1: FPN1 is used.
 *  ISP2: FPN1 is used.
 */

struct ia_css_fpn_table {
	/*
	 * Table content (fixed patterns noise).
	 * u0.[13-shift], [0,63]
	 */
	s16 *data;
	/*
	 * Table width (in pixels).
	 * This is the input frame width.
	 */
	u32 width;
	/*
	 * Table height (in pixels).
	 * This is the input frame height.
	 */
	u32 height;
	/*
	 * Common exponent of table content.
	 * u8.0, [0,13]
	 */
	u32 shift;
	/*
	 * Fpn is enabled.
	 * bool
	 */
	u32 enabled;
};

struct ia_css_fpn_configuration {
	const struct ia_css_frame_info *info;
};

#endif /* __IA_CSS_FPN_TYPES_H */
