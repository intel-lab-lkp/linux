/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIPS_H
#define _FIPS_H

#include <linux/bits.h>

#ifdef CONFIG_CRYPTO_FIPS
extern int fips_enabled;
extern struct atomic_notifier_head fips_fail_notif_chain;

void fips_fail_notify(void);
/*
 * fips_allows_exception - check if a FIPS exception is active
 * Returns true when FIPS is enabled and the requested exception
 * bit is set via the fips_exception= boot parameter.
 */
int fips_allows_exception(unsigned long feature);

#else
#define fips_enabled 0

static inline void fips_fail_notify(void) {}
static inline int fips_allows_exception(unsigned long feature)
{
	return 0;
}

#endif

#define FIPS_EXCEPTION_WIFI_MFP	BIT(0)

#endif
