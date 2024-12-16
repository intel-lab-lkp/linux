/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* bits.h: Macros for dealing with bitmasks.  */

#ifndef _UAPI_LINUX_BITS_H
#define _UAPI_LINUX_BITS_H

/* Result is '(1u << (hi + 1)) - (1u << lo)' coded to avoid overflow. */
#define ___GENMASK(one, hi, lo) \
	((((one) << (hi)) - 1) * 2 + 1 - (((one) << (lo)) - 1))

#define __GENMASK(hi, lo) ___GENMASK(1UL, hi, lo)

#define __GENMASK_ULL(hi, lo) ___GENMASK(1ULL, hi, lo)

#define __GENMASK_U128(hi, lo) ___GENMASK((unsigned __int128)1, hi, lo)

#endif /* _UAPI_LINUX_BITS_H */
