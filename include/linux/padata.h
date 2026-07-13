/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * padata.h - header for the padata parallelization interface
 *
 * Copyright (C) 2008, 2009 secunet Security Networks AG
 * Copyright (C) 2008, 2009 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * Copyright (c) 2020 Oracle and/or its affiliates.
 * Author: Daniel Jordan <daniel.m.jordan@oracle.com>
 */

#ifndef PADATA_H
#define PADATA_H

#include <linux/init.h>
#include <linux/types.h>

/**
 * struct padata_mt_job - represents one multithreaded job
 *
 * @thread_fn: Called for each chunk of work that a padata thread does.
 * @fn_arg: The thread function argument.
 * @start: The start of the job (units are job-specific).
 * @size: size of this node's work (units are job-specific).
 * @align: Ranges passed to the thread function fall on this boundary, with the
 *         possible exceptions of the beginning and end of the job.
 * @min_chunk: The minimum chunk size in job-specific units.  This allows
 *             the client to communicate the minimum amount of work that's
 *             appropriate for one worker thread to do at once.
 * @max_threads: Max threads to use for the job, actual number may be less
 *               depending on task size and minimum chunk size.
 * @numa_aware: Distribute jobs to different nodes with CPU in a round robin fashion.
 */
struct padata_mt_job {
	void (*thread_fn)(unsigned long start, unsigned long end, void *arg);
	void			*fn_arg;
	unsigned long		start;
	unsigned long		size;
	unsigned long		align;
	unsigned long		min_chunk;
	int			max_threads;
	bool			numa_aware;
};

#ifdef CONFIG_PADATA
extern void __init padata_init(void);
extern void __init padata_do_multithreaded(struct padata_mt_job *job);
#else
static inline void __init padata_init(void) {}
static inline void __init padata_do_multithreaded(struct padata_mt_job *job)
{
	job->thread_fn(job->start, job->start + job->size, job->fn_arg);
}
#endif

#endif
