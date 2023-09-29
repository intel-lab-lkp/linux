/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */
#ifndef __INTEL_FRACTIONAL_HELPERS_H__
#define __INTEL_FRACTIONAL_HELPERS_H__

 /*
  * Convert a U6.4 fixed-point bits-per-pixel (bpp) value to an integer bpp value.
  */
static inline int intel_fractional_bpp_from_x16(int bpp_x16)
{
	return bpp_x16 >> 4;
}

/*
 * Extract the fractional part of a U6.4 fixed-point bpp value based on the
 * last 4 bits representing fractional bits, obtained by multiplying by 10000
 * and then dividing by 16, as the bpp value is initially left-shifted by 4
 * to allocate 4 bits for the fractional part.
 */
static inline int intel_fractional_bpp_decimal(int bpp_x16)
{
	return (bpp_x16 & 0xf) * 625;
}

/*
 * Convert bits-per-pixel (bpp) to a U6.4 fixed-point representation.
 */
static inline int intel_fractional_bpp_to_x16(int bpp)
{
	return bpp << 4;
}

#endif /*  __INTEL_FRACTIONAL_HELPERS_H__ */

