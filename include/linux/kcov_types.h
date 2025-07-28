/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KCOV_STATE_H
#define _LINUX_KCOV_STATE_H

#ifdef CONFIG_KCOV
/* See kernel/kcov.c for more details. */
struct kcov_state {
	/* Size of the area (in long's). */
	unsigned int size;

	/* Buffer for coverage collection, shared with the userspace. */
	void *area;

	/*
	 * KCOV sequence number: incremented each time kcov is reenabled, used
	 * by kcov_remote_stop(), see the comment there.
	 */
	int sequence;
};
#endif /* CONFIG_KCOV */

#endif /* _LINUX_KCOV_STATE_H */
