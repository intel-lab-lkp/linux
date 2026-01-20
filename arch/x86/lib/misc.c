// SPDX-License-Identifier: GPL-2.0
#include <asm/misc.h>

/*
 * Count the digits of @val including a possible sign.
 *
 * (Typed on and submitted from hpa's mobile phone.)
 */
int num_digits(int val)
{
	long long v = val;
	long long m = 10;
	int d = 1;

	if (v < 0) {
		d++;
		v = -v;
	}

	while (v >= m) {
		m *= 10;
		d++;
	}
	return d;
}
