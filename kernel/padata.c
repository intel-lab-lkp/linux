// SPDX-License-Identifier: GPL-2.0
/*
 * padata.c - generic interface to run multithreaded jobs
 *
 * See Documentation/core-api/padata.rst for more information.
 *
 * Copyright (C) 2008, 2009 secunet Security Networks AG
 * Copyright (C) 2008, 2009 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * Copyright (c) 2020 Oracle and/or its affiliates.
 * Author: Daniel Jordan <daniel.m.jordan@oracle.com>
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/nodemask.h>
#include <linux/padata.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define	PADATA_WORK_ONSTACK	1	/* Work's memory is on stack */

struct padata_work {
	struct work_struct	pw_work;
	struct list_head	pw_list;  /* padata_free_works linkage */
	void			*pw_data;
};

static DEFINE_SPINLOCK(padata_works_lock);
static struct padata_work *padata_works;
static LIST_HEAD(padata_free_works);

struct padata_mt_job_state {
	spinlock_t		lock;
	struct completion	completion;
	struct padata_mt_job	*job;
	int			nworks;
	int			nworks_fini;
	unsigned long		chunk_size;
};

static void __init padata_mt_helper(struct work_struct *work);

static struct padata_work *padata_work_alloc(void)
{
	struct padata_work *pw;

	lockdep_assert_held(&padata_works_lock);

	if (list_empty(&padata_free_works))
		return NULL;	/* No more work items allowed to be queued. */

	pw = list_first_entry(&padata_free_works, struct padata_work, pw_list);
	list_del(&pw->pw_list);
	return pw;
}

/*
 * This function is marked __ref because this function may be optimized in such
 * a way that it directly refers to work_fn's address, which causes modpost to
 * complain when work_fn is marked __init. This scenario was observed with clang
 * LTO, where padata_work_init() was optimized to refer directly to
 * padata_mt_helper() because the calls to padata_work_init() with other work_fn
 * values were eliminated or inlined.
 */
static void __ref padata_work_init(struct padata_work *pw, work_func_t work_fn,
				   void *data, int flags)
{
	if (flags & PADATA_WORK_ONSTACK)
		INIT_WORK_ONSTACK(&pw->pw_work, work_fn);
	else
		INIT_WORK(&pw->pw_work, work_fn);
	pw->pw_data = data;
}

static int __init padata_work_alloc_mt(int nworks, void *data,
				       struct list_head *head)
{
	int i;

	spin_lock_bh(&padata_works_lock);
	/* Start at 1 because the current task participates in the job. */
	for (i = 1; i < nworks; ++i) {
		struct padata_work *pw = padata_work_alloc();

		if (!pw)
			break;
		padata_work_init(pw, padata_mt_helper, data, 0);
		list_add(&pw->pw_list, head);
	}
	spin_unlock_bh(&padata_works_lock);

	return i;
}

static void padata_work_free(struct padata_work *pw)
{
	lockdep_assert_held(&padata_works_lock);
	list_add(&pw->pw_list, &padata_free_works);
}

static void __init padata_works_free(struct list_head *works)
{
	struct padata_work *cur, *next;

	if (list_empty(works))
		return;

	spin_lock_bh(&padata_works_lock);
	list_for_each_entry_safe(cur, next, works, pw_list) {
		list_del(&cur->pw_list);
		padata_work_free(cur);
	}
	spin_unlock_bh(&padata_works_lock);
}

static void __init padata_mt_helper(struct work_struct *w)
{
	struct padata_work *pw = container_of(w, struct padata_work, pw_work);
	struct padata_mt_job_state *ps = pw->pw_data;
	struct padata_mt_job *job = ps->job;
	bool done;

	spin_lock(&ps->lock);

	while (job->size > 0) {
		unsigned long start, size, end;

		start = job->start;
		/* So end is chunk size aligned if enough work remains. */
		size = roundup(start + 1, ps->chunk_size) - start;
		size = min(size, job->size);
		end = start + size;

		job->start = end;
		job->size -= size;

		spin_unlock(&ps->lock);
		job->thread_fn(start, end, job->fn_arg);
		spin_lock(&ps->lock);
	}

	++ps->nworks_fini;
	done = (ps->nworks_fini == ps->nworks);
	spin_unlock(&ps->lock);

	if (done)
		complete(&ps->completion);
}

/**
 * padata_do_multithreaded - run a multithreaded job
 * @job: Description of the job.
 *
 * See the definition of struct padata_mt_job for more details.
 */
void __init padata_do_multithreaded(struct padata_mt_job *job)
{
	/* In case threads finish at different times. */
	static const unsigned long load_balance_factor = 4;
	struct padata_work my_work, *pw;
	struct padata_mt_job_state ps;
	LIST_HEAD(works);
	int nworks, nid;
	static atomic_t last_used_nid __initdata;

	if (job->size == 0)
		return;

	/* Ensure at least one thread when size < min_chunk. */
	nworks = max(job->size / max(job->min_chunk, job->align), 1ul);
	nworks = min(nworks, job->max_threads);

	if (nworks == 1) {
		/* Single thread, no coordination needed, cut to the chase. */
		job->thread_fn(job->start, job->start + job->size, job->fn_arg);
		return;
	}

	spin_lock_init(&ps.lock);
	init_completion(&ps.completion);
	ps.job	       = job;
	ps.nworks      = padata_work_alloc_mt(nworks, &ps, &works);
	ps.nworks_fini = 0;

	/*
	 * Chunk size is the amount of work a helper does per call to the
	 * thread function.  Load balance large jobs between threads by
	 * increasing the number of chunks, guarantee at least the minimum
	 * chunk size from the caller, and honor the caller's alignment.
	 * Ensure chunk_size is at least 1 to prevent divide-by-0
	 * panic in padata_mt_helper().
	 */
	ps.chunk_size = job->size / (ps.nworks * load_balance_factor);
	ps.chunk_size = max(ps.chunk_size, job->min_chunk);
	ps.chunk_size = max(ps.chunk_size, 1ul);
	ps.chunk_size = roundup(ps.chunk_size, job->align);

	list_for_each_entry(pw, &works, pw_list)
		if (job->numa_aware) {
			int old_node = atomic_read(&last_used_nid);

			do {
				nid = next_node_in(old_node, node_states[N_CPU]);
			} while (!atomic_try_cmpxchg(&last_used_nid, &old_node, nid));
			queue_work_node(nid, system_dfl_wq, &pw->pw_work);
		} else {
			queue_work(system_dfl_wq, &pw->pw_work);
		}

	/* Use the current thread, which saves starting a workqueue worker. */
	padata_work_init(&my_work, padata_mt_helper, &ps, PADATA_WORK_ONSTACK);
	padata_mt_helper(&my_work.pw_work);

	/* Wait for all the helpers to finish. */
	wait_for_completion(&ps.completion);

	destroy_work_on_stack(&my_work.pw_work);
	padata_works_free(&works);
}

void __init padata_init(void)
{
	unsigned int i, possible_cpus;

	possible_cpus = num_possible_cpus();
	padata_works = kmalloc_objs(struct padata_work, possible_cpus);
	if (!padata_works) {
		pr_warn("padata: initialization failed\n");
		return;
	}

	for (i = 0; i < possible_cpus; ++i)
		list_add(&padata_works[i].pw_list, &padata_free_works);
}
