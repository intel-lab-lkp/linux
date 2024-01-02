/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_MULTI_CLOCK_H
#define _UAPI_MULTI_CLOCK_H

#include <linux/types.h>
#include <linux/time_types.h>

#define MULTI_PTP_MAX_CLOCKS 32 /* Max number of clocks */
#define MULTI_PTP_MAX_SAMPLES 32 /* Max allowed offset measurement samples. */

struct __ptp_multi_clock_get {
	unsigned int n_clocks; /* Desired number of clocks. */
	unsigned int n_samples; /* Desired number of measurements per clock. */
	clockid_t clkid_arr[MULTI_PTP_MAX_CLOCKS]; /* list of clock IDs */
	/*
	 * Array of list of n_clocks clocks time samples n_samples times.
	 */
	struct  __kernel_timespec ts[MULTI_PTP_MAX_SAMPLES][MULTI_PTP_MAX_CLOCKS];
};

#endif /* _UAPI_MULTI_CLOCK_H */
