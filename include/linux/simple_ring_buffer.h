/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SIMPLE_RING_BUFFER_H
#define _LINUX_SIMPLE_RING_BUFFER_H

#include <linux/list.h>
#include <linux/ring_buffer.h>
#include <linux/ring_buffer_types.h>
#include <linux/types.h>

/*
 * Ideally those struct would stay private but the caller needs to know
 * the allocation size for simple_ring_buffer_init().
 */
struct simple_buffer_page {
	struct list_head	link;
	struct buffer_data_page	*page;
	u64			entries;
	u32			write;
	u32			id;
};

struct simple_rb_per_cpu {
	struct simple_buffer_page	*tail_page;
	struct simple_buffer_page	*reader_page;
	struct simple_buffer_page	*head_page;
	struct simple_buffer_page	*bpages;
	struct trace_buffer_meta	*meta;
	u32				nr_pages;

#define SIMPLE_RB_UNAVAILABLE	0
#define SIMPLE_RB_READY		1
#define SIMPLE_RB_WRITING	2
	u32				status;

	u64				last_overrun;
	u64				write_stamp;

	struct simple_rb_cbs		*cbs;
};

/**
 * simple_ring_buffer_init - Init @cpu_buffer based on @desc
 *
 * @cpu_buffer:	A simple_rb_per_cpu buffer to init, allocated by the caller.
 * @bpages:	Array of simple_buffer_pages, with as many elements as @desc->nr_page_va
 * @desc:	A ring_buffer_desc
 *
 * Returns: 0 on success or -EINVAL if the content of @desc is invalid
 */
int simple_ring_buffer_init(struct simple_rb_per_cpu *cpu_buffer, struct simple_buffer_page *bpages,
			    const struct ring_buffer_desc *desc);

/**
 * simple_ring_buffer_unload - Prepare @cpu_buffer for deletion
 *
 * @cpu_buffer:	A simple_rb_per_cpu that will be deleted.
 */
void simple_ring_buffer_unload(struct simple_rb_per_cpu *cpu_buffer);

/**
 * simple_ring_buffer_reserve - Reserve an entry in @cpu_buffer
 *
 * @cpu_buffer:	A simple_rb_per_cpu
 * @length:	Size of the entry in bytes
 * @timestamp:	Timestamp of the entry
 *
 * Returns the address of the entry where to write data or NULL
 */
void *simple_ring_buffer_reserve(struct simple_rb_per_cpu *cpu_buffer, unsigned long length,
				 u64 timestamp);

/**
 * simple_ring_buffer_commit - Commit the entry reserved with simple_ring_buffer_reserve()
 *
 * @cpu_buffer:	The simple_rb_per_cpu where the entry has been reserved
 */
void simple_ring_buffer_commit(struct simple_rb_per_cpu *cpu_buffer);

/**
 * simple_ring_buffer_enable_tracing - Enable or disable writing to @cpu_buffer
 *
 * @cpu_buffer: A simple_rb_per_cpu
 * @enable:	True to enable tracing, False to disable it
 *
 * Returns 0 on success or -ENODEV if @cpu_buffer was unloaded
 */
int simple_ring_buffer_enable_tracing(struct simple_rb_per_cpu *cpu_buffer, bool enable);

/**
 * simple_ring_buffer_reset - Reset @cpu_buffer
 *
 * @cpu_buffer: A simple_rb_per_cpu
 *
 * This will not clear the content of the data, only reset counters and pointers
 *
 * Returns 0 on success or -ENODEV if @cpu_buffer was unloaded.
 */
int simple_ring_buffer_reset(struct simple_rb_per_cpu *cpu_buffer);

/**
 * simple_ring_buffer_swap_reader_page - Swap ring-buffer head with the reader
 *
 * This function enables consuming reading. It ensures the current head page will not be overwritten
 * and can be safely read.
 *
 * @cpu_buffer: A simple_rb_per_cpu
 *
 * Returns 0 on success, -ENODEV if @cpu_buffer was unloaded or -EBUSY if we failed to catch the
 * head page.
 */
int simple_ring_buffer_swap_reader_page(struct simple_rb_per_cpu *cpu_buffer);

#endif
