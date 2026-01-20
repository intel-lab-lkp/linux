// SPDX-License-Identifier: GPL-2.0
#include <asm/misc.h>

/*
 * Count the digits of @val including a possible sign.
 */
int num_digits(int val)
{
	unsigned int v;
	int d;

	if (val < 0) {
		d = 1;
		v = -val;
	} else {
		d = 0;
		v = val;
	}

	if (v <= 9)
		return d + 1;
	if (v <= 99)
		return d + 2;
	if (v <= 999)
		return d + 3;
	if (v <= 9999)
		return d + 4;
	if (v <= 99999)
		return d + 5;
	if (v <= 999999)
		return d + 6;
	if (v <= 9999999)
		return d + 7;
	if (v <= 99999999)
		return d + 8;
	if (v <= 999999999)
		return d + 9;

	return d + 10;
}
