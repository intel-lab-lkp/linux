/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Snippet to be included in rv_trace.h
 */

#ifdef CONFIG_RV_MON_RTS
DEFINE_EVENT(event_ltl_monitor_cpu, event_rts,
	TP_PROTO(unsigned int cpu, char *states, char *atoms, char *next),
	TP_ARGS(cpu, states, atoms, next));

DEFINE_EVENT(error_ltl_monitor_cpu, error_rts,
	TP_PROTO(unsigned int cpu),
	TP_ARGS(cpu));
#endif /* CONFIG_RV_MON_RTS */
