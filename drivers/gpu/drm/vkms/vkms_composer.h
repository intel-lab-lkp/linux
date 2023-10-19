/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_COMPOSER_H_
#define _VKMS_COMPOSER_H_

#include "vkms_drv.h"

s64 get_lut_index(const struct vkms_color_lut *lut, u16 channel_value);
u16 lerp_u16(u16 a, u16 b, s64 t);

/*
 * This enum is related to the positions of the variables inside
 * `struct drm_color_lut`, so the order of both needs to be the same.
 */
enum lut_channel {
	LUT_RED = 0,
	LUT_GREEN,
	LUT_BLUE,
	LUT_RESERVED
};

u16 apply_lut_to_channel_value(const struct vkms_color_lut *lut, u16 channel_value,
			       enum lut_channel channel);

#endif /* _VKMS_COMPOSER_H_ */