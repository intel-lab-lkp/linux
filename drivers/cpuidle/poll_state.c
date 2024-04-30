// SPDX-License-Identifier: GPL-2.0-only
/*
 * poll_state.c - Polling idle state
 */

#include <linux/cpuidle.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/idle.h>

#ifdef CONFIG_ARM64
/*
 * POLL_IDLE_RELAX_COUNT determines how often we check for timeout
 * while polling for TIF_NEED_RESCHED in thread_info->flags.
 *
 * Set this to a low value since arm64, instead of polling, uses a
 * event based mechanism.
 */
#define POLL_IDLE_RELAX_COUNT	1
#else
#define POLL_IDLE_RELAX_COUNT	200
#endif

static int __cpuidle poll_idle(struct cpuidle_device *dev,
			       struct cpuidle_driver *drv, int index)
{
	u64 time_start;

	time_start = local_clock_noinstr();

	dev->poll_time_limit = false;

	raw_local_irq_enable();
	if (!current_set_polling_and_test()) {
		unsigned int loop_count;
		u64 limit;

		limit = cpuidle_poll_time(drv, dev);

		while (!need_resched()) {
			loop_count = 0;
			if (local_clock_noinstr() - time_start > limit) {
				dev->poll_time_limit = true;
				break;
			}

			smp_cond_load_relaxed(&current_thread_info()->flags,
					      VAL & _TIF_NEED_RESCHED ||
					      loop_count++ >= POLL_IDLE_RELAX_COUNT);
		}
	}
	raw_local_irq_disable();

	current_clr_polling();

	return index;
}

void cpuidle_poll_state_init(struct cpuidle_driver *drv)
{
	struct cpuidle_state *state = &drv->states[0];

	snprintf(state->name, CPUIDLE_NAME_LEN, "POLL");
	snprintf(state->desc, CPUIDLE_DESC_LEN, "CPUIDLE CORE POLL IDLE");
	state->exit_latency = 0;
	state->target_residency = 0;
	state->exit_latency_ns = 0;
	state->target_residency_ns = 0;
	state->power_usage = -1;
	state->enter = poll_idle;
	state->flags = CPUIDLE_FLAG_POLLING;
}
EXPORT_SYMBOL_GPL(cpuidle_poll_state_init);
