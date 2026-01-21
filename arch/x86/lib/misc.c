// SPDX-License-Identifier: GPL-2.0
#include <asm/misc.h>

/*
 * Count the decimal digits of an unsigned integer.
 */
int num_digits(unsigned int x)
{
	int n = 0;

	asm("cmp %2,%1; sbb $-2,%0" : "+r" (n) : "r" (x), "g" (10));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (100));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (1000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (10000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (100000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (1000000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (10000000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (100000000));
	asm("cmp %2,%1; sbb $-1,%0" : "+r" (n) : "r" (x), "g" (1000000000));

	return n;
}
