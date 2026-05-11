/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Snippet to be included in rv_trace.h for tlob tracepoints.
 *
 * event_tlob and error_tlob are defined on the event_da_monitor_id and
 * error_da_monitor_id classes, following the same pattern as nomiss.
 * error_env_tlob carries the environment variable name that caused the
 * clock-invariant violation (budget exceeded).
 * The id field carries the pid of the monitored task.
 */

#ifdef CONFIG_RV_MON_TLOB
/* id is the pid of the monitored task */
DEFINE_EVENT(event_da_monitor_id, event_tlob,
	     TP_PROTO(int id, char *state, char *event, char *next_state, bool final_state),
	     TP_ARGS(id, state, event, next_state, final_state));

DEFINE_EVENT(error_da_monitor_id, error_tlob,
	     TP_PROTO(int id, char *state, char *event),
	     TP_ARGS(id, state, event));

DEFINE_EVENT(error_env_da_monitor_id, error_env_tlob,
	     TP_PROTO(int id, char *state, char *event, char *env),
	     TP_ARGS(id, state, event, env));

/*
 * detail_env_tlob - per-state time breakdown emitted alongside error_env_tlob.
 *
 * Fired once per budget violation, immediately after error_env_tlob, from
 * the hrtimer callback (hardirq context).  The three _ns fields sum to
 * approximately threshold_us * 1000; any rounding comes from the partial
 * time accumulated in the current state since the last transition.
 */
TRACE_EVENT(detail_env_tlob,
	TP_PROTO(int pid, u64 threshold_us,
		 u64 running_ns, u64 waiting_ns, u64 sleeping_ns),
	TP_ARGS(pid, threshold_us, running_ns, waiting_ns, sleeping_ns),
	TP_STRUCT__entry(
		__field(int,	pid)
		__field(u64,	threshold_us)
		__field(u64,	running_ns)
		__field(u64,	waiting_ns)
		__field(u64,	sleeping_ns)
	),
	TP_fast_assign(
		__entry->pid		= pid;
		__entry->threshold_us	= threshold_us;
		__entry->running_ns	= running_ns;
		__entry->waiting_ns	= waiting_ns;
		__entry->sleeping_ns	= sleeping_ns;
	),
	TP_printk("pid=%d threshold_us=%llu running_ns=%llu waiting_ns=%llu sleeping_ns=%llu",
		  __entry->pid, __entry->threshold_us,
		  __entry->running_ns, __entry->waiting_ns,
		  __entry->sleeping_ns)
);
#endif /* CONFIG_RV_MON_TLOB */
