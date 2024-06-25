/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */

/*
 * This structure provides a vDSO-style clock to VM guests, exposing the
 * relationship (or lack thereof) between the CPU clock (TSC, timebase, arch
 * counter, etc.) and real time. It is designed to address the problem of
 * live migration, which other clock enlightenments do not.
 *
 * When a guest is live migrated, this affects the clock in two ways.
 *
 * First, even between identical hosts the actual frequency of the underlying
 * counter will change within the tolerances of its specification (typically
 * ±50PPM, or 4 seconds a day). The frequency also varies over time on the
 * same host, but can be tracked by NTP as it generally varies slowly. With
 * live migration there is a step change in the frequency, with no warning.
 *
 * Second, there may be a step change in the value of the counter itself, as
 * its accuracy is limited by the precision of the NTP synchronization on the
 * source and destination hosts.
 *
 * So any calibration (NTP, PTP, etc.) which the guest has done on the source
 * host before migration is invalid, and needs to be redone on the new host.
 *
 * In its most basic mode, this structure provides only an indication to the
 * guest that live migration has occurred. This allows the guest to know that
 * its clock is invalid and take remedial action. For applications that need
 * reliable accurate timestamps (e.g. distributed databases), the structure
 * can be mapped all the way to userspace. This allows the application to see
 * directly for itself that the clock is disrupted and take appropriate
 * action, even when using a vDSO-style method to get the time instead of a
 * system call.
 *
 * In its more advanced mode. this structure can also be used to expose the
 * precise relationship of the CPU counter to real time, as calibrated by the
 * host. This means that userspace applications can have accurate time
 * immediately after live migration, rather than having to pause operations
 * and wait for NTP to recover. This mode does, of course, rely on the
 * counter being reliable and consistent across CPUs.
 *
 * Note that this must be true UTC, never with smeared leap seconds. If a
 * guest wishes to construct a smeared clock, it can do so. Presenting a
 * smeared clock through this interface would be problematic because it
 * actually messes with the apparent counter *period*. A linear smearing
 * of 1 ms per second would effectively tweak the counter period by 1000PPM
 * at the start/end of the smearing period, while a sinusoidal smear would
 * basically be impossible to represent.
 */

#ifndef __VMCLOCK_H__
#define __VMCLOCK_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

struct vmclock_abi {
	uint32_t magic;
#define VMCLOCK_MAGIC	0x4b4c4356 /* "VCLK" */
	uint16_t size;		/* Size of page containing this structure */
	uint16_t version;	/* 1 */

	/* Sequence lock. Low bit means an update is in progress. */
	uint64_t seq_count;

	/*
	 * This field changes to another non-repeating value when the CPU
	 * counter is disrupted, for example on live migration.
	 */
	uint64_t disruption_marker;

	/*
	 * By providing the TAI offset, the guest can know both UTC and TAI
	 * reliably. There is no need to choose one *or* the other. Valid if
	 * VMCLOCK_FLAG_TAI_OFFSET_VALID is set in flags.
	 */
	int16_t tai_offset_sec;

	uint16_t flags;
	/* Indicates that the tai_offset_sec field is valid */
#define VMCLOCK_FLAG_TAI_OFFSET_VALID		(1 << 0)
	/*
	 * Optionally used to notify guests of pending maintenance events.
	 * A guest may wish to remove itself from service if an event is
	 * coming up. Two flags indicate the rough imminence of the event.
	 */
#define VMCLOCK_FLAG_DISRUPTION_SOON		(1 << 1) /* About a day */
#define VMCLOCK_FLAG_DISRUPTION_IMMINENT	(1 << 2) /* About an hour */
	/* Indicates that the utc_time_maxerror_picosec field is valid */
#define VMCLOCK_FLAG_UTC_MAXERROR_VALID		(1 << 3)
	/* Indicates counter_period_error_rate_frac_sec is valid */
#define VMCLOCK_FLAG_UTC_PERIOD_ERROR_VALID	(1 << 4)

	uint8_t clock_status;
#define VMCLOCK_STATUS_UNKNOWN		0
#define VMCLOCK_STATUS_INITIALIZING	1
#define VMCLOCK_STATUS_SYNCHRONIZED	2
#define VMCLOCK_STATUS_FREERUNNING	3
#define VMCLOCK_STATUS_UNRELIABLE	4

	uint8_t counter_id;
#define VMCLOCK_COUNTER_INVALID		0
#define VMCLOCK_COUNTER_X86_TSC		1
#define VMCLOCK_COUNTER_ARM_VCNT	2

	/* Bit shift for counter_period_frac_sec and its error rate */
	uint8_t counter_period_shift;

	/*
	 * Unlike in NTP, this can indicate a leap second in the past. This
	 * is needed to allow guests to derive an imprecise clock with
	 * smeared leap seconds for themselves, as some modes of smearing
	 * need the adjustments to continue even after the moment at which
	 * the leap second should have occurred.
	 */
	int8_t leapsecond_direction;
	uint64_t leapsecond_tai_sec; /* Since 1970-01-01 00:00:00z */

	/*
	 * Paired values of counter and UTC at a given point in time.
	 */
	uint64_t counter_value;
	uint64_t utc_time_sec; /* Since 1970-01-01 00:00:00z */
	uint64_t utc_time_frac_sec;

	/*
	 * Counter frequency, and error margin. The unit of these fields is
	 * seconds >> (64 + counter_period_shift)
	 */
	uint64_t counter_period_frac_sec;
	uint64_t counter_period_error_rate_frac_sec;

	/* Error margin of UTC reading above (± picoseconds) */
	uint64_t utc_time_maxerror_picosec;
};

#endif /*  __VMCLOCK_H__ */
