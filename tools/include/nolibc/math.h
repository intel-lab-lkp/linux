/* SPDX-License-Identifier: LGPL-2.1 OR MIT */
/*
 * math definitions for NOLIBC
 * Copyright (C) 2025 Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 */

#ifndef _NOLIBC_SYS_MATH_H
#define _NOLIBC_SYS_MATH_H

static __inline__
double fabs(double x)
{
	return x >= 0 ? x : -x;
}

static __inline__
float fabsf(float x)
{
	return x >= 0 ? x : -x;
}

static __inline__
long double fabsl(long double x)
{
	return x >= 0 ? x : -x;
}

/* make sure to include all global symbols */
#include "nolibc.h"

#endif /* _NOLIBC_SYS_MATH_H */
