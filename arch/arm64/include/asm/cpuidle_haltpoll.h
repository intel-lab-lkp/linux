/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_HALTPOLL_H
#define _ASM_HALTPOLL_H

static inline void arch_haltpoll_enable(unsigned int cpu)
{
}

static inline void arch_haltpoll_disable(unsigned int cpu)
{
}

static inline bool arch_haltpoll_supported(void)
{
	/*
	 * Ensure the event stream is available to provide a terminating
	 * condition to the WFE in the poll loop.
	 */
	return arch_timer_evtstrm_available();
}
#endif
