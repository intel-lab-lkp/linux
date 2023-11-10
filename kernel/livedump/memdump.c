// SPDX-License-Identifier: GPL-2.0-or-later
/* memdump.c - Live Dump's memory dumping management
 * Copyright (C) 2012 Hitachi, Ltd.
 * Copyright (C) 2023 SUSE
 * Author: YOSHIDA Masanori <masanori.yoshida.tv@hitachi.com>
 * Author: Lukas Hruska <lhruska@suse.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "memdump.h"

#define CREATE_TRACE_POINTS
#include "memdump_trace.h"

#include <asm/wrprotect.h>

#include <linux/crash_core.h>
#include <linux/crash_dump.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/kexec.h>
#include <linux/kfifo.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/sizes.h>
#include <linux/printk.h>
#include <linux/tracepoint.h>

#define MEMDUMP_KFIFO_SIZE	131072 /* in pages */
#define SECTOR_SHIFT		9
#define PFN_ELF_0			0
#define PFN_ELF_1			1

static const char THREAD_NAME[] = "livedump";
static struct block_device *memdump_bdev;

/* ELF metadata */
static unsigned char *vmcoreinfo;
static void *elf_data;
static unsigned long elf_size;
static struct crash_mem *cmem;

/* ELF modification */
static char *elfnotes_buf;
static size_t elfnotes_sz;

/***** State machine *****/
enum MEMDUMP_STATE {
	_MEMDUMP_INIT,
	MEMDUMP_INACTIVE = _MEMDUMP_INIT,
	MEMDUMP_ACTIVATING,
	MEMDUMP_ACTIVE,
	MEMDUMP_INACTIVATING,
	_MEMDUMP_OVERFLOW,
};

static struct memdump_state {
	atomic_t val;
	atomic_t count;
	spinlock_t lock;
} __aligned(PAGE_SIZE) memdump_state = {
	ATOMIC_INIT(_MEMDUMP_INIT),
	ATOMIC_INIT(0),
	__SPIN_LOCK_INITIALIZER(memdump_state.lock),
};

/* memdump_state_inc
 *
 * Increments ACTIVE state refcount.
 * The refcount must be zero to transit to next state (INACTIVATING).
 */
static bool memdump_state_inc(void)
{
	bool ret;

	spin_lock(&memdump_state.lock);
	ret = (atomic_read(&memdump_state.val) == MEMDUMP_ACTIVE);
	if (ret)
		atomic_inc(&memdump_state.count);
	spin_unlock(&memdump_state.lock);
	return ret;
}

/* memdump_state_dec
 *
 * Decrements ACTIVE state refcount
 */
static void memdump_state_dec(void)
{
	atomic_dec(&memdump_state.count);
}

/* memdump_state_transit
 *
 * Transit to next state.
 * If current state isn't assumed state, transition fails.
 */
static bool memdump_state_transit(enum MEMDUMP_STATE assumed)
{
	bool ret;

	spin_lock(&memdump_state.lock);
	ret = (atomic_read(&memdump_state.val) == assumed &&
		atomic_read(&memdump_state.count) == 0);
	if (ret) {
		atomic_inc(&memdump_state.val);
		if (atomic_read(&memdump_state.val) == _MEMDUMP_OVERFLOW)
			atomic_set(&memdump_state.val, _MEMDUMP_INIT);
	}
	spin_unlock(&memdump_state.lock);
	return ret;
}

static void memdump_state_transit_back(void)
{
	atomic_dec(&memdump_state.val);
}

/***** Request queue *****/

/*
 * Request queue consists of 2 kfifos: pend, pool
 *
 * Processing between the two kfifos:
 *  (1)handle_page READs one request from POOL.
 *  (2)handle_page makes the request and WRITEs it to PEND.
 *  (3)kthread READs the request from PEND and submits bio.
 *  (4)endio WRITEs the request to POOL.
 *
 * kfifo permits parallel access by 1 reader and 1 writer.
 * Therefore, (1), (2) and (4) must be serialized.
 * (3) need not be protected since livedump uses only one kthread.
 *
 * (1) is protected by pool_r_lock.
 * (2) is protected by pend_w_lock.
 * (4) is protected by pool_w_lock.
 */

struct memdump_request {
	void *p; /* pointing to buffer (one page) */
	unsigned long pfn;
};

static struct memdump_request_queue {
	void *pages[MEMDUMP_KFIFO_SIZE];

	STRUCT_KFIFO(struct memdump_request, MEMDUMP_KFIFO_SIZE) pool;
	STRUCT_KFIFO(struct memdump_request, MEMDUMP_KFIFO_SIZE) pend;

	spinlock_t pool_w_lock;
	spinlock_t pool_r_lock;
	spinlock_t pend_w_lock;
} __aligned(PAGE_SIZE) memdump_req_queue, memdump_req_queue_for_sweep;

static void free_req_queue(void)
{
	int i;

	for (i = 0; i < MEMDUMP_KFIFO_SIZE; i++) {
		if (memdump_req_queue.pages[i]) {
			free_page((unsigned long)memdump_req_queue.pages[i]);
			memdump_req_queue.pages[i] = NULL;
		}
	}
	for (i = 0; i < MEMDUMP_KFIFO_SIZE; i++) {
		if (memdump_req_queue_for_sweep.pages[i]) {
			free_page((unsigned long)memdump_req_queue_for_sweep.pages[i]);
			memdump_req_queue_for_sweep.pages[i] = NULL;
		}
	}
}

static long alloc_req_queue(void)
{
	long ret;
	int i;
	struct memdump_request req;

	/* initialize spinlocks */
	spin_lock_init(&memdump_req_queue.pool_w_lock);
	spin_lock_init(&memdump_req_queue.pool_r_lock);
	spin_lock_init(&memdump_req_queue.pend_w_lock);
	spin_lock_init(&memdump_req_queue_for_sweep.pool_w_lock);
	spin_lock_init(&memdump_req_queue_for_sweep.pool_r_lock);
	spin_lock_init(&memdump_req_queue_for_sweep.pend_w_lock);

	/* initialize kfifos */
	INIT_KFIFO(memdump_req_queue.pend);
	INIT_KFIFO(memdump_req_queue.pool);
	INIT_KFIFO(memdump_req_queue_for_sweep.pend);
	INIT_KFIFO(memdump_req_queue_for_sweep.pool);

	/* allocate pages and push pages into pool */
	for (i = 0; i < MEMDUMP_KFIFO_SIZE; i++) {
		/* for normal queue */
		memdump_req_queue.pages[i]
			= (void *)__get_free_page(GFP_KERNEL);
		if (!memdump_req_queue.pages[i]) {
			ret = -ENOMEM;
			goto err;
		}

		req.p = memdump_req_queue.pages[i];
		ret = kfifo_put(&memdump_req_queue.pool, req);
		BUG_ON(!ret);

		/* for sweep queue */
		memdump_req_queue_for_sweep.pages[i]
			= (void *)__get_free_page(GFP_KERNEL);
		if (!memdump_req_queue_for_sweep.pages[i]) {
			ret = -ENOMEM;
			goto err;
		}

		req.p = memdump_req_queue_for_sweep.pages[i];
		ret = kfifo_put(&memdump_req_queue_for_sweep.pool, req);
		BUG_ON(!ret);
	}

	return 0;

err:
	free_req_queue();
	return ret;
}

/***** Kernel thread *****/
static struct memdump_thread {
	struct task_struct *tsk;
	bool is_active;
	struct completion completion;
	wait_queue_head_t waiters;
} __aligned(PAGE_SIZE) memdump_thread;

static int memdump_thread_func(void *);

static long start_memdump_thread(void)
{
	memdump_thread.is_active = true;
	init_completion(&memdump_thread.completion);
	init_waitqueue_head(&memdump_thread.waiters);
	memdump_thread.tsk = kthread_run(
			memdump_thread_func, NULL, THREAD_NAME);
	if (IS_ERR(memdump_thread.tsk))
		return PTR_ERR(memdump_thread.tsk);
	return 0;
}

static void stop_memdump_thread(void)
{
	memdump_thread.is_active = false;
	wait_for_completion(&memdump_thread.completion);
}

static void memdump_endio(struct bio *bio)
{
	struct memdump_request req = { .p = page_address(bio_page(bio)) };
	struct memdump_request_queue *queue = (bio->bi_private ?
			&memdump_req_queue_for_sweep : &memdump_req_queue);

	spin_lock(&queue->pool_w_lock);
	kfifo_put(&queue->pool, req);
	spin_unlock(&queue->pool_w_lock);

	wake_up(&memdump_thread.waiters);
}

static int memdump_thread_func(void *_)
{
	struct bio *bio;
	struct memdump_request req;

	do {
		/* Process request */
		while (kfifo_get(&memdump_req_queue.pend, &req)) {
			bio = bio_alloc(memdump_bdev, 1, REQ_OP_WRITE, GFP_KERNEL);

			if (WARN_ON(!bio)) {
				spin_lock(&memdump_req_queue.pool_w_lock);
				kfifo_put(&memdump_req_queue.pool, req);
				spin_unlock(&memdump_req_queue.pool_w_lock);
				continue;
			}

			bio->bi_bdev = memdump_bdev;
			bio->bi_end_io = memdump_endio;
			bio->bi_iter.bi_sector = req.pfn << (PAGE_SHIFT - SECTOR_SHIFT);
			bio_add_page(bio, virt_to_page(req.p), PAGE_SIZE, 0);

			trace_memdump_bio_submit(memdump_bdev, req.pfn);

			submit_bio(bio);
		}

		/* Process request for sweep*/
		while (kfifo_get(&memdump_req_queue_for_sweep.pend, &req)) {
			bio = bio_alloc(memdump_bdev, 1, REQ_OP_WRITE, GFP_KERNEL);

			if (WARN_ON(!bio)) {
				spin_lock(&memdump_req_queue_for_sweep.pool_w_lock);
				kfifo_put(&memdump_req_queue_for_sweep.pool, req);
				spin_unlock(&memdump_req_queue_for_sweep.pool_w_lock);
				continue;
			}

			bio->bi_bdev = memdump_bdev;
			bio->bi_end_io = memdump_endio;
			bio->bi_iter.bi_sector = req.pfn << (PAGE_SHIFT - SECTOR_SHIFT);
			bio->bi_private = (void *)1; /* for sweep */
			bio_add_page(bio, virt_to_page(req.p), PAGE_SIZE, 0);

			trace_memdump_bio_submit(memdump_bdev, req.pfn);

			submit_bio(bio);
		}

		msleep(20);
	} while (memdump_thread.is_active);

	complete(&memdump_thread.completion);
	return 0;
}

static int select_pages(void);

int livedump_memdump_init(const char *bdevpath)
{
	long ret;

	if (WARN(!memdump_state_transit(MEMDUMP_INACTIVE),
				"livedump: memdump is already initialized.\n"))
		return -EBUSY;

	/* Get bdev */
	ret = -ENOENT;
	memdump_bdev = blkdev_get_by_path(bdevpath, FMODE_EXCL, &memdump_bdev);
	if (memdump_bdev < 0)
		goto err;

	/* Allocate request queue */
	ret = alloc_req_queue();
	if (ret)
		goto err_bdev;

	/* Start thread */
	ret = start_memdump_thread();
	if (ret)
		goto err_freeq;

	/* Select target pages */
	select_pages();

	/* Allocate space for vmcore info */
	vmcoreinfo = vmalloc(PAGE_SIZE);
	cmem = vzalloc(struct_size(cmem, ranges, 1));
	if (WARN_ON(!vmcoreinfo || !cmem))
		return -ENOMEM;

	memdump_state_transit(MEMDUMP_ACTIVATING); /* always succeeds */
	return 0;

err_freeq:
	free_req_queue();
err_bdev:
	blkdev_put(memdump_bdev, FMODE_EXCL);
err:
	memdump_state_transit_back();
	return ret;
}

void livedump_memdump_uninit(void)
{
	if (!memdump_state_transit(MEMDUMP_ACTIVE))
		return;

	/* Stop thread */
	stop_memdump_thread();

	/* Free request queue */
	free_req_queue();

	/* Free vmcoreinfo */
	if (vmcoreinfo)
		vunmap(vmcoreinfo);
	if (cmem)
		vfree(cmem);

	/* merged notes */
	if (elfnotes_buf)
		vfree(elfnotes_buf);

	/* Put bdev */
	blkdev_put(memdump_bdev, FMODE_EXCL);

	memdump_state_transit(MEMDUMP_INACTIVATING); /* always succeeds */
}

void livedump_memdump_handle_page(unsigned long pfn, unsigned long addr, int for_sweep)
{
	int ret;
	unsigned long flags;
	struct memdump_request req;
	struct memdump_request_queue *queue =
		(for_sweep ? &memdump_req_queue_for_sweep : &memdump_req_queue);
	DEFINE_WAIT(wait);

	BUG_ON(addr & ~PAGE_MASK);

	if (!memdump_state_inc())
		return;

	/* Get buffer */
retry_after_wait:
	spin_lock_irqsave(&queue->pool_r_lock, flags);
	ret = kfifo_get(&queue->pool, &req);
	spin_unlock_irqrestore(&queue->pool_r_lock, flags);

	if (!ret) {
		if (WARN_ON_ONCE(!for_sweep))
			goto err;
		else {
			prepare_to_wait(&memdump_thread.waiters, &wait,
					TASK_UNINTERRUPTIBLE);
			schedule();
			finish_wait(&memdump_thread.waiters, &wait);
			goto retry_after_wait;
		}
	}

	/* Make request */
	req.pfn = pfn;
	if (pfn == PFN_ELF_0) {
		memcpy(req.p, elf_data, elf_size);
		memset(req.p + elf_size, 0, PAGE_SIZE - elf_size);
	} else if (pfn == PFN_ELF_1)
		memcpy(req.p, elfnotes_buf, PAGE_SIZE);
	else
		memcpy(req.p, (void *)addr, PAGE_SIZE);

	/* Queue request */
	spin_lock_irqsave(&queue->pend_w_lock, flags);
	kfifo_put(&queue->pend, req);
	spin_unlock_irqrestore(&queue->pend_w_lock, flags);

err:
	memdump_state_dec();
}

/* select_pages
 *
 * Eliminate pages that contain memdump's stuffs from bitmap.
 */
static int select_pages(void)
{
	unsigned long i;

	/* Unselect memdump stuffs */
	wrprotect_unselect_pages(
			(unsigned long)&memdump_state, sizeof(memdump_state));
	wrprotect_unselect_pages(
			(unsigned long)&memdump_req_queue,
			sizeof(memdump_req_queue));
	wrprotect_unselect_pages(
			(unsigned long)&memdump_req_queue_for_sweep,
			sizeof(memdump_req_queue_for_sweep));
	wrprotect_unselect_pages(
			(unsigned long)&memdump_thread, sizeof(memdump_thread));
	for (i = 0; i < MEMDUMP_KFIFO_SIZE; i++) {
		wrprotect_unselect_pages((unsigned long)memdump_req_queue.pages[i],
		    PAGE_ALIGN(sizeof(struct memdump_request)));
		wrprotect_unselect_pages((unsigned long)memdump_req_queue_for_sweep.pages[i],
		    PAGE_ALIGN(sizeof(struct memdump_request)));
		cond_resched();
	}

	return 0;
}

void livedump_memdump_sm_init(void)
{
	unsigned int cpu;
	struct pt_regs regs;

	memset(&regs, 0, sizeof(struct pt_regs));
	regs.ip = (unsigned long)memdump_thread_func;

	for_each_present_cpu(cpu) {
		crash_save_cpu(&regs, cpu);
	}

	cmem->max_nr_ranges = 1;
	cmem->nr_ranges = 1;
	cmem->ranges[0].start = SZ_1M;
	cmem->ranges[0].end = ((max_pfn + 1) << PAGE_SHIFT) - 1;
	crash_update_vmcoreinfo_safecopy(vmcoreinfo);
	crash_save_vmcoreinfo();
	crash_prepare_elf64_headers(cmem, 1, &elf_data, &elf_size);
	crash_update_vmcoreinfo_safecopy(NULL);
	merge_note_headers_elf64((char *)elf_data, &elf_size, &elfnotes_buf, &elfnotes_sz);
}

void livedump_memdump_write_elf_hdr(void)
{
	livedump_memdump_handle_page(PFN_ELF_0, 0, 1);
	livedump_memdump_handle_page(PFN_ELF_1, 0, 1);
}
