// SPDX-License-Identifier: GPL-2.0
#include <asm/misc.h>

/*
 * Count the digits of @val including a possible sign.
 */
int num_digits(int val)
{
	unsigned int v = val;
	int d = 0;

	if (val < 0) {
		d = 1;
		v = -v;
	}

	switch (v) {
	case 0 ... 9:
		return d + 1;
	case 10 ... 99:
		return d + 2;
	case 100 ... 999:
		return d + 3;
	case 1000 ... 9999:
		return d + 4;
	case 10000 ... 99999:
		return d + 5;
	case 100000 ... 999999:
		return d + 6;
	case 1000000 ... 9999999:
		return d + 7;
	case 10000000 ... 99999999:
		return d + 8;
	case 100000000 ... 999999999:
		return d + 9;
	default:
		return d + 10;
	}
}
