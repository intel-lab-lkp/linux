/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Snippet to be included in rv_trace.h
 */

#ifdef CONFIG_RV_MON_TLOB
/*
 * tlob uses the generic event_da_monitor_id and error_da_monitor_id event
 * classes so that both event classes are instantiated.  This avoids a
 * -Werror=unused-variable warning that the compiler emits when a
 * DECLARE_EVENT_CLASS has no corresponding DEFINE_EVENT instance.
 *
 * The event_tlob tracepoint is defined here but the call-site in
 * da_handle_event() is overridden with a no-op macro below so that no
 * trace record is emitted on every scheduler context switch.  Budget
 * violations are reported via the dedicated tlob_budget_exceeded event.
 *
 * error_tlob IS kept active so that invalid DA transitions (programming
 * errors) are still visible in the ftrace ring buffer for debugging.
 */
DEFINE_EVENT(event_da_monitor_id, event_tlob,
	     TP_PROTO(int id, char *state, char *event, char *next_state,
		      bool final_state),
	     TP_ARGS(id, state, event, next_state, final_state));

DEFINE_EVENT(error_da_monitor_id, error_tlob,
	     TP_PROTO(int id, char *state, char *event),
	     TP_ARGS(id, state, event));

/*
 * Override the trace_event_tlob() call-site with a no-op after the
 * DEFINE_EVENT above has satisfied the event class instantiation
 * requirement.  The tracepoint symbol itself exists (and can be enabled
 * via tracefs) but the automatic call from da_handle_event() is silenced
 * to avoid per-context-switch ftrace noise during normal operation.
 */
#undef trace_event_tlob
#define trace_event_tlob(id, state, event, next_state, final_state)	\
	do { (void)(id); (void)(state); (void)(event);			\
	     (void)(next_state); (void)(final_state); } while (0)
#endif /* CONFIG_RV_MON_TLOB */
