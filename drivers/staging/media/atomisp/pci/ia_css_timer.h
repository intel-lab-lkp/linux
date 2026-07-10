/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2010 - 2015, Intel Corporation.
 */

#ifndef __IA_CSS_TIMER_H
#define __IA_CSS_TIMER_H

/*
 * Timer interface definitions.
 */
#include <type_support.h>       /* for uint32_t */
#include "ia_css_err.h"

/* Timer reading definition. */
typedef u32 clock_value_t;

/* 32 bit clock tick (timestamp based on timer-value of CSS-internal timer). */
struct ia_css_clock_tick {
	clock_value_t ticks; /* Measured time in ticks. */
};

/* TIMER event codes. */
enum ia_css_tm_event {
	IA_CSS_TM_EVENT_AFTER_INIT,
	/* Timer event after initialization. */
	IA_CSS_TM_EVENT_MAIN_END,
	/* Timer event after end of main. */
	IA_CSS_TM_EVENT_THREAD_START,
	/* Timer event after thread start. */
	IA_CSS_TM_EVENT_FRAME_PROC_START,
	/* Timer event after frame process start. */
	IA_CSS_TM_EVENT_FRAME_PROC_END
	/* Timer event after frame process end. */
};

/* Code measurement common struct. */
struct ia_css_time_meas {
	clock_value_t start_timer_value; /* Measured time in ticks. */
	clock_value_t end_timer_value;   /* Measured time in ticks. */
};

/*
 * SIZE_OF_IA_CSS_CLOCK_TICK_STRUCT
 * Checks to ensure correct alignment for struct ia_css_clock_tick.
 */
#define SIZE_OF_IA_CSS_CLOCK_TICK_STRUCT sizeof(clock_value_t)

/* Checks to ensure correct alignment for ia_css_time_meas. */
#define SIZE_OF_IA_CSS_TIME_MEAS_STRUCT (sizeof(clock_value_t) \
					+ sizeof(clock_value_t))

/**
 * ia_css_timer_get_current_tick() - API to fetch timer count directly.
 * @curr_ts: [out] Measured count value.
 *
 * Return: 0 if success.
 */
int ia_css_timer_get_current_tick(struct ia_css_clock_tick *curr_ts);

#endif  /* __IA_CSS_TIMER_H */
