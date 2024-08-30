// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/cpuidle_haltpoll.h>

bool arch_haltpoll_want(bool force)
{
	/*
	 * Enabling haltpoll requires two things:
	 *
	 * - Event stream support to provide a terminating condition to the
	 *   WFE in the poll loop.
	 *
	 * - KVM support for arch_haltpoll_enable(), arch_haltpoll_disable().
	 *
	 * Given that the second is missing, allow haltpoll to only be force
	 * loaded.
	 */
	return (arch_timer_evtstrm_available() && false) || force;
}
EXPORT_SYMBOL_GPL(arch_haltpoll_want);
