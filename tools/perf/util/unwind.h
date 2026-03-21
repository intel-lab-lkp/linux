/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UNWIND_H
#define __UNWIND_H

#include <linux/compiler.h>
#include <linux/types.h>
#include "util/map_symbol.h"

struct maps;
struct perf_sample;
struct thread;

struct unwind_entry {
	struct map_symbol ms;
	u64		  ip;
};

typedef int (*unwind_entry_cb_t)(struct unwind_entry *entry, void *arg);

#ifdef HAVE_DWARF_UNWIND_SUPPORT
/*
 * When best_effort is set, don't report errors and fail silently. This could
 * be expanded in the future to be more permissive about things other than
 * error messages.
 */
int unwind__get_entries(unwind_entry_cb_t cb, void *arg,
			struct thread *thread,
			struct perf_sample *data, int max_stack,
			bool best_effort);
#else
static inline int
unwind__get_entries(unwind_entry_cb_t cb __maybe_unused,
		    void *arg __maybe_unused,
		    struct thread *thread __maybe_unused,
		    struct perf_sample *data __maybe_unused,
		    int max_stack __maybe_unused,
		    bool best_effort __maybe_unused)
{
	return 0;
}
#endif /* HAVE_DWARF_UNWIND_SUPPORT */

#endif /* __UNWIND_H */
