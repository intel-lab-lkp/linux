/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KCOV_STATE_H
#define _LINUX_KCOV_STATE_H

#ifdef CONFIG_KCOV
/* See kernel/kcov.c for more details. */
struct kcov_state {
	/* Size of the area (in long's). */
	unsigned int size;
	/*
	 * Pointer to user-provided memory used by kcov. This memory may
	 * contain multiple buffers.
	 */
	void *area;

	/* Size of the trace (in long's). */
	unsigned int trace_size;
	/* Buffer for coverage collection, shared with the userspace. */
	unsigned long *trace;

	/*
	 * KCOV sequence number: incremented each time kcov is reenabled, used
	 * by kcov_remote_stop(), see the comment there.
	 */
	int sequence;
};
#endif /* CONFIG_KCOV */

#endif /* _LINUX_KCOV_STATE_H */
