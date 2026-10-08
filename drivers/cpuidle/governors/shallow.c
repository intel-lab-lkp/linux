// SPDX-License-Identifier: GPL-2.0
/*
 * shallow.c - the shallow and shallow_nopoll idle governors
 *
 * Always select the shallowest enabled idle state, trading idle energy
 * savings for the lowest wakeup latency the platform can offer.  The
 * shallow_nopoll one skips polling states unless PM QoS requires them.
 */

#include <linux/cpuidle.h>
#include <linux/init.h>

/**
 * shallow_select - select the shallowest enabled idle state
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 * @stop_tick: indication on whether or not to stop the tick
 *
 * The state selected here is the least energy-efficient one offered by the
 * driver, so there is no point in looking at either the timer events or the
 * PM QoS latency limits: no constraint can justify going shallower than that.
 * Leave the tick running, as the CPU is not expected to stay idle for long.
 *
 * Return: the index of the first idle state not disabled for @dev, or 0 if
 * all of them are disabled.
 */
static int shallow_select(struct cpuidle_driver *drv,
			  struct cpuidle_device *dev, bool *stop_tick)
{
	int i;

	*stop_tick = false;

	/*
	 * cpuidle_enter_state() does not check whether the state it is asked
	 * to enter has been disabled, so the per-state "disable" attributes
	 * need to be honoured here.  If all of the states are disabled, fall
	 * back to the first one, like the other governors do.
	 */
	for (i = 0; i < drv->state_count; i++)
		if (!dev->states_usage[i].disable)
			return i;

	return 0;
}

/**
 * shallow_nopoll_select - select the shallowest enabled non-polling idle state
 * @drv: cpuidle driver containing state data
 * @dev: the CPU
 * @stop_tick: indication on whether or not to stop the tick
 *
 * Same as shallow_select(), but skip the polling states.  Unlike them, the
 * shallowest non-polling state may take longer to leave than the PM QoS
 * latency limit allows, so fall back to shallow_select() in that case, as
 * well as when all of the non-polling states are disabled.
 *
 * Return: the index of the first idle state that is neither disabled for @dev
 * nor a polling one, or what shallow_select() returns in the cases above.
 */
static int shallow_nopoll_select(struct cpuidle_driver *drv,
				 struct cpuidle_device *dev, bool *stop_tick)
{
	int i;

	for (i = 0; i < drv->state_count; i++) {
		if (dev->states_usage[i].disable ||
		    (drv->states[i].flags & CPUIDLE_FLAG_POLLING))
			continue;

		if (drv->states[i].exit_latency_ns >
		    cpuidle_governor_latency_req(dev->cpu))
			break;

		*stop_tick = false;
		return i;
	}

	return shallow_select(drv, dev, stop_tick);
}

static struct cpuidle_governor shallow_governor = {
	.name =		"shallow",
	.rating =	1,
	.select =	shallow_select,
};

static struct cpuidle_governor shallow_nopoll_governor = {
	.name =		"shallow_nopoll",
	.rating =	1,
	.select =	shallow_nopoll_select,
};

static int __init init_shallow(void)
{
	int ret = cpuidle_register_governor(&shallow_governor);

	if (ret)
		return ret;

	return cpuidle_register_governor(&shallow_nopoll_governor);
}

postcore_initcall(init_shallow);
