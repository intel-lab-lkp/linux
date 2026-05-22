/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TRACE_STACKMAP_H
#define _TRACE_STACKMAP_H

#include <linux/types.h>
#include <linux/atomic.h>

#define FTRACE_STACKMAP_MAX_DEPTH	64

/* Binary export format */
#define FTRACE_STACKMAP_BIN_MAGIC	0x464D5342	/* 'FSMB' */
#define FTRACE_STACKMAP_BIN_VERSION	2

struct ftrace_stackmap_bin_header {
	u32 magic;
	u32 version;
	u32 nr_stacks;
	u32 reserved;
};

struct ftrace_stackmap_bin_entry {
	u32 stack_id;
	u32 nr;
	u32 ref_count;
	u32 reserved;
	/* followed by u64 ips[nr] */
};

struct trace_array;

#ifdef CONFIG_FTRACE_STACKMAP

struct ftrace_stackmap;

struct ftrace_stackmap *ftrace_stackmap_create(struct trace_array *tr);
void ftrace_stackmap_destroy(struct ftrace_stackmap *smap);
int ftrace_stackmap_get_id(struct ftrace_stackmap *smap,
			   unsigned long *ips, unsigned int nr_entries);
int ftrace_stackmap_reset(struct ftrace_stackmap *smap);

extern const struct file_operations ftrace_stackmap_fops;
extern const struct file_operations ftrace_stackmap_stat_fops;
extern const struct file_operations ftrace_stackmap_bin_fops;

#else

struct ftrace_stackmap;
static inline struct ftrace_stackmap *ftrace_stackmap_create(struct trace_array *tr) { return NULL; }
static inline void ftrace_stackmap_destroy(struct ftrace_stackmap *s) { }
static inline int ftrace_stackmap_get_id(struct ftrace_stackmap *s,
					 unsigned long *ips, unsigned int n)
{ return -ENOSYS; }
static inline int ftrace_stackmap_reset(struct ftrace_stackmap *s) { return 0; }

#endif
#endif /* _TRACE_STACKMAP_H */
