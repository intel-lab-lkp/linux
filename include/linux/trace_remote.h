/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_TRACE_REMOTE_H
#define _LINUX_TRACE_REMOTE_H

#include <linux/ring_buffer.h>

struct trace_remote_callbacks {
	struct trace_buffer_desc *
		(*load_trace_buffer)(unsigned long size, void *priv);
	void	(*unload_trace_buffer)(struct trace_buffer_desc *desc, void *priv);
	int	(*enable_tracing)(bool enable, void *priv);
	int	(*swap_reader_page)(unsigned int cpu, void *priv);
	int	(*reset)(unsigned int cpu, void *priv);
};

int trace_remote_register(const char *name, struct trace_remote_callbacks *cbs, void *priv);
int trace_remote_alloc_buffer(struct trace_buffer_desc *desc, size_t size,
			      const struct cpumask *cpumask);
void trace_remote_free_buffer(struct trace_buffer_desc *desc);

#endif
