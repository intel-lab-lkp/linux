/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_HRTIMER_TYPES_H
#define _LINUX_HRTIMER_TYPES_H

#include <linux/types.h>
#include <linux/timerqueue_types.h>

struct hrtimer;
struct hrtimer_clock_base;

/*
 * Return values for the callback function
 */
enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART,	/* Timer must be restarted */
};

/**
 * struct hrtimer_forward_args - deferred forward request of an expiry
 *				 injecting callback
 * @now:	forward past this time
 * @interval:	the interval to forward by
 *
 * Filled in by an expiry injecting callback (see hrtimer_setup_ext())
 * which returns HRTIMER_RESTART. The hrtimer core applies the forward
 * with the timer base lock held before requeueing the timer, i.e.
 * hrtimer_forward(timer, now, interval).
 */
struct hrtimer_forward_args {
	ktime_t				now;
	ktime_t				interval;
};

/* Callback function of an expiry injecting timer, see hrtimer_setup_ext() */
typedef enum hrtimer_restart (*hrtimer_ext_func_t)(struct hrtimer *timer, ktime_t expires,
						   struct hrtimer_forward_args *fwd);

/**
 * struct hrtimer - the basic hrtimer structure
 * @node:	Linked timerqueue node, which also manages node.expires,
 *		the absolute expiry time in the hrtimers internal
 *		representation. The time is related to the clock on
 *		which the timer is based. Is setup by adding
 *		slack to the _softexpires value. For non range timers
 *		identical to _softexpires.
 * @_softexpires: the absolute earliest expiry time of the hrtimer.
 *		The time which was given as expiry time when the timer
 *		was armed.
 * @function:	timer expiry callback function
 * @function_ext: expiry injecting timer callback function, receives the
 *		expiry snapshot and returns a forward request instead of
 *		modifying the expiry itself. Valid if @is_ext is set.
 * @base:	pointer to the timer base (per cpu and per clock)
 * @is_queued:	Indicates whether a timer is enqueued or not
 * @is_rel:	Set if the timer was armed relative
 * @is_soft:	Set if hrtimer will be expired in soft interrupt context.
 * @is_hard:	Set if hrtimer will be expired in hard interrupt context
 *		even on RT.
 * @is_lazy:	Set if the timer is frequently rearmed to avoid updates
 *		of the clock event device
 * @is_ext:	Set if @function_ext is valid instead of @function
 *
 * The hrtimer structure must be initialized by hrtimer_setup() or
 * hrtimer_setup_ext()
 */
struct hrtimer {
	struct timerqueue_linked_node	node;
	struct hrtimer_clock_base	*base;
	bool				is_queued;
	bool				is_rel;
	bool				is_soft;
	bool				is_hard;
	bool				is_lazy;
	bool				is_ext;
	ktime_t				_softexpires;
	union {
		enum hrtimer_restart	(*__private function)(struct hrtimer *);
		hrtimer_ext_func_t	__private function_ext;
	};
};

#endif /* _LINUX_HRTIMER_TYPES_H */
