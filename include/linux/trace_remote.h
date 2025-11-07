/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_TRACE_REMOTE_H
#define _LINUX_TRACE_REMOTE_H

#include <linux/dcache.h>
#include <linux/ring_buffer.h>

/**
 * struct trace_remote_callbacks - Callbacks used by Tracefs to control the remote
 *
 * @init:		Called once the remote has been registered. Allows the
 *			caller to extend the Tracefs remote directory
 * @load_trace_buffer:  Called before Tracefs accesses the trace buffer for the first
 *			time. Must return a &trace_buffer_desc
 *			(most likely filled with trace_remote_alloc_buffer())
 * @unload_trace_buffer:
 *			Called once Tracefs has no use for the trace buffer
 *			(most likely call trace_remote_free_buffer())
 * @enable_tracing:	Called on Tracefs tracing_on. It is expected from the
 *			remote to allow writing.
 * @swap_reader_page:	Called when Tracefs consumes a new page from a
 *			ring-buffer. It is expected from the remote to isolate a
 * @reset:		Called on `echo 0 > trace`. It is expected from the
 *			remote to reset all ring-buffer pages.
 *			new reader-page from the @cpu ring-buffer.
 */
struct trace_remote_callbacks {
	int	(*init)(struct dentry *d, void *priv);
	struct trace_buffer_desc *(*load_trace_buffer)(unsigned long size, void *priv);
	void	(*unload_trace_buffer)(struct trace_buffer_desc *desc, void *priv);
	int	(*enable_tracing)(bool enable, void *priv);
	int	(*swap_reader_page)(unsigned int cpu, void *priv);
	int	(*reset)(unsigned int cpu, void *priv);
};

/**
 * trace_remote_register() - Register a Tracefs remote
 *
 * A trace remote is an entity, outside of the kernel (most likely firmware or
 * hypervisor) capable of writing events into a Tracefs compatible ring-buffer.
 * The kernel would then act as a reader.
 *
 * The registered remote will be found under the Tracefs directory
 * remotes/<name>.
 *
 * @name:	Name of the remote, used for the Tracefs remotes/ directory.
 * @cbs:	Set of callbacks used to control the remote.
 * @priv:	Private data, passed to each callback from @cbs.
 * @events:	Array of events. &remote_event.name and &remote_event.id must be
 *		filled by the caller.
 * @nr_events:	Number of events in the @events array.
 *
 * Return: 0 on success, negative error code on failure.
 */
int trace_remote_register(const char *name, struct trace_remote_callbacks *cbs, void *priv);

/**
 * trace_remote_alloc_buffer() - Dynamically allocate a trace buffer
 *
 * Helper to dynamically allocate a set of pages (enough to cover @buffer_size)
 * for each CPU from @cpumask and fill @desc. Most likely called from
 * &trace_remote_callbacks.load_trace_buffer.
 *
 * @desc:		Uninitialized trace_buffer_desc
 * @desc_size:		Size of the trace_buffer_desc. Must be at least equal to
 *			trace_buffer_desc_size()
 * @buffer_size:	Size in bytes of each per-CPU ring-buffer
 * @cpumask:		CPUs to allocate a ring-buffer for
 *
 * Return: 0 on success, negative error code on failure.
 */
int trace_remote_alloc_buffer(struct trace_buffer_desc *desc, size_t desc_size, size_t buffer_size,
			      const struct cpumask *cpumask);

/**
 * trace_remote_free_buffer() - Free trace buffer allocated with
 *				trace_remote_alloc_buffer()
 *
 * Most likely called from &trace_remote_callbacks.unload_trace_buffer.
 *
 * @desc:	Descriptor of the per-CPU ring-buffers, originally filled by
 *		trace_remote_alloc_buffer()
 */
void trace_remote_free_buffer(struct trace_buffer_desc *desc);

#endif
