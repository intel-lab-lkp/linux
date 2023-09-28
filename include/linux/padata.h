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

#include <linux/refcount.h>
#include <linux/compiler_types.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kobject.h>


/**
 * struct padata_priv - Represents one job
 *
 * @info: Used to pass information from the parallel to the serial function.
 * @parallel_done: completetion of parallel_work.
 * @parallel_work: Parallel work.
 * @serial_work: Serial work.
 * @parallel: Parallel execution function.
 * @serial: Serial complete function.
 */
struct padata_priv {
	int			info;
	struct completion parallel_done;
	struct work_struct	parallel_work;
	struct work_struct	serial_work;
	void                    (*parallel)(struct padata_priv *padata);
	void                    (*serial)(struct padata_priv *padata);
};

/**
 * struct padata_shell - Wrapper around struct parallel_data, its
 * purpose is to allow the underlying control structure to be replaced
 * on the fly using RCU.
 *
 * @pinst: padat instance.
 * @list: List entry in padata_instance list.
 */
struct padata_shell {
	struct padata_instance		*pinst;
	struct list_head		list;
};

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
 */
struct padata_mt_job {
	void (*thread_fn)(unsigned long start, unsigned long end, void *arg);
	void			*fn_arg;
	unsigned long		start;
	unsigned long		size;
	unsigned long		align;
	unsigned long		min_chunk;
	int			max_threads;
};

/**
 * struct padata_instance - The overall control structure.
 *
 * @parallel_wq: The workqueue used for parallel work.
 * @serial_wq: The workqueue used for serial work.
 * @pslist: List of padata_shell objects attached to this instance.
 * @kobj: padata instance kernel object.
 * @lock: padata instance lock.
 * @flags: padata flags.
 */
struct padata_instance {
	struct workqueue_struct		*parallel_wq;
	struct workqueue_struct		*serial_wq;
	struct list_head		pslist;
	struct mutex			 lock;
	u8				 flags;
#define	PADATA_INIT	1
#define	PADATA_RESET	2
#define	PADATA_INVALID	4
};

#ifdef CONFIG_PADATA
extern void __init padata_init(void);
#else
static inline void __init padata_init(void) {}
#endif

extern struct padata_instance *padata_alloc(const char *name);
extern void padata_free(struct padata_instance *pinst);
extern struct padata_shell *padata_alloc_shell(struct padata_instance *pinst);
extern void padata_free_shell(struct padata_shell *ps);
extern int padata_do_parallel(struct padata_shell *ps,
			      struct padata_priv *padata);
extern void __init padata_do_multithreaded(struct padata_mt_job *job);
#endif
