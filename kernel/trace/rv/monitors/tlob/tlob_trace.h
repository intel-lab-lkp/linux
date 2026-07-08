/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Snippet to be included in rv_trace.h
 */

#ifdef CONFIG_RV_MON_TLOB
DEFINE_EVENT(event_da_monitor_id, event_tlob,
	     TP_PROTO(int id, char *state, char *event,
		      char *next_state, bool final_state),
	     TP_ARGS(id, state, event, next_state, final_state));

DEFINE_EVENT(error_da_monitor_id, error_tlob,
	     TP_PROTO(int id, char *state, char *event),
	     TP_ARGS(id, state, event));

DEFINE_EVENT(error_env_da_monitor_id, error_env_tlob,
	     TP_PROTO(int id, char *state, char *event, char *env),
	     TP_ARGS(id, state, event, env));

/*
 * detail_env_tlob - per-state latency breakdown emitted on budget violation.
 *
 * Fired immediately after error_env_tlob from the hrtimer callback.
 * Fields show how much time was spent in each DA state since tlob_start_task().
 * running_ns + waiting_ns + sleeping_ns approximately equals total
 * elapsed time (threshold_ns exceeded).
 */
TRACE_EVENT(detail_env_tlob,
	TP_PROTO(int id, u64 threshold_ns,
		 u64 running_ns, u64 waiting_ns, u64 sleeping_ns),
	TP_ARGS(id, threshold_ns, running_ns, waiting_ns, sleeping_ns),
	TP_STRUCT__entry(
		__field(int,	id)
		__field(u64,	threshold_ns)
		__field(u64,	running_ns)
		__field(u64,	waiting_ns)
		__field(u64,	sleeping_ns)
	),
	TP_fast_assign(
		__entry->id		= id;
		__entry->threshold_ns	= threshold_ns;
		__entry->running_ns	= running_ns;
		__entry->waiting_ns	= waiting_ns;
		__entry->sleeping_ns	= sleeping_ns;
	),
	TP_printk("pid=%d threshold_ns=%llu"
		  " running_ns=%llu waiting_ns=%llu sleeping_ns=%llu",
		__entry->id, __entry->threshold_ns,
		__entry->running_ns, __entry->waiting_ns, __entry->sleeping_ns)
);
#endif /* CONFIG_RV_MON_TLOB */
