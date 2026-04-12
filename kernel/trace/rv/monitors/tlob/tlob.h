/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _RV_TLOB_H
#define _RV_TLOB_H

/*
 * C representation of the tlob automaton, generated from tlob.dot via rvgen
 * and extended with tlob_start_task()/tlob_stop_task() declarations.
 * For the format description see Documentation/trace/rv/deterministic_automata.rst
 */

#include <linux/rv.h>
#include <uapi/linux/rv.h>

#define MONITOR_NAME tlob

enum states_tlob {
	unmonitored_tlob,
	on_cpu_tlob,
	off_cpu_tlob,
	state_max_tlob,
};

#define INVALID_STATE state_max_tlob

enum events_tlob {
	trace_start_tlob,
	switch_in_tlob,
	switch_out_tlob,
	sched_wakeup_tlob,
	trace_stop_tlob,
	budget_expired_tlob,
	event_max_tlob,
};

struct automaton_tlob {
	char *state_names[state_max_tlob];
	char *event_names[event_max_tlob];
	unsigned char function[state_max_tlob][event_max_tlob];
	unsigned char initial_state;
	bool final_states[state_max_tlob];
};

static const struct automaton_tlob automaton_tlob = {
	.state_names = {
		"unmonitored",
		"on_cpu",
		"off_cpu",
	},
	.event_names = {
		"trace_start",
		"switch_in",
		"switch_out",
		"sched_wakeup",
		"trace_stop",
		"budget_expired",
	},
	.function = {
		/* unmonitored */
		{
			on_cpu_tlob,		/* trace_start    */
			unmonitored_tlob,	/* switch_in      */
			unmonitored_tlob,	/* switch_out     */
			unmonitored_tlob,	/* sched_wakeup   */
			INVALID_STATE,		/* trace_stop     */
			INVALID_STATE,		/* budget_expired */
		},
		/* on_cpu */
		{
			INVALID_STATE,		/* trace_start    */
			INVALID_STATE,		/* switch_in      */
			off_cpu_tlob,		/* switch_out     */
			on_cpu_tlob,		/* sched_wakeup   */
			unmonitored_tlob,	/* trace_stop     */
			unmonitored_tlob,	/* budget_expired */
		},
		/* off_cpu */
		{
			INVALID_STATE,		/* trace_start    */
			on_cpu_tlob,		/* switch_in      */
			off_cpu_tlob,		/* switch_out     */
			off_cpu_tlob,		/* sched_wakeup   */
			unmonitored_tlob,	/* trace_stop     */
			unmonitored_tlob,	/* budget_expired */
		},
	},
	/*
	 * final_states: unmonitored is the sole accepting state.
	 * Violations are recorded via ntf_push and tlob_budget_exceeded.
	 */
	.initial_state = unmonitored_tlob,
	.final_states = { 1, 0, 0 },
};

/* Exported for use by the RV ioctl layer (rv_dev.c) */
int tlob_start_task(struct task_struct *task, u64 threshold_us,
		    struct file *notify_file, u64 tag);
int tlob_stop_task(struct task_struct *task);

/* Maximum number of concurrently monitored tasks (also used by KUnit). */
#define TLOB_MAX_MONITORED	64U

/*
 * Ring buffer constants (also published in UAPI for mmap size calculation).
 */
#define TLOB_RING_DEFAULT_CAP	64U	/* records allocated at open()  */
#define TLOB_RING_MIN_CAP	 8U	/* minimum accepted by mmap()   */
#define TLOB_RING_MAX_CAP	4096U	/* maximum accepted by mmap()   */

/**
 * struct tlob_ring - per-fd mmap-capable violation ring buffer.
 *
 * Allocated as a contiguous page range at rv_open() time:
 *   page 0:    struct tlob_mmap_page  (shared with userspace)
 *   pages 1-N: struct tlob_event[capacity]
 */
struct tlob_ring {
	struct tlob_mmap_page	*page;
	struct tlob_event	*data;
	u32			 mask;
	spinlock_t		 lock;
	unsigned long		 base;
	unsigned int		 order;
};

/**
 * struct rv_file_priv - per-fd private data for /dev/rv.
 */
struct rv_file_priv {
	struct tlob_ring	ring;
	wait_queue_head_t	waitq;
};

#if IS_ENABLED(CONFIG_KUNIT)
int tlob_init_monitor(void);
void tlob_destroy_monitor(void);
int tlob_enable_hooks(void);
void tlob_disable_hooks(void);
void tlob_event_push_kunit(struct rv_file_priv *priv,
			  const struct tlob_event *info);
int tlob_parse_uprobe_line(char *buf, u64 *thr_out,
			   char **path_out,
			   loff_t *start_out, loff_t *stop_out);
#endif /* CONFIG_KUNIT */

#endif /* _RV_TLOB_H */
