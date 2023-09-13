// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics target POLLING queue thread implementation.
 * Copyright (c) 2023 Ping Gan.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <linux/refcount.h>
#include <linux/bits.h>
#include <linux/blk-mq.h>
#include <linux/kthread.h>

#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/bio.h>
#include <linux/net.h>
#include <linux/wait.h>
#include "nvmet.h"


struct nvmet_pqt_sqe {
	struct bio					*bio;
	struct nvmet_pqt_bio_req	*pqt_req;
	struct list_head			list;
};

enum {
	NVMET_PQ_THREAD_SHOULD_STOP = 0,
	NVMET_PQ_THREAD_ENABLE = 1,
};

struct nvmet_pq_ring_headtail {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

struct nvmet_pq_ring {
	struct nvmet_pq_ring_headtail prod, cons;
	u32 size;
	u32 mask;
	u32 capacity;
	struct bio_list *qe_arry[] ____cacheline_aligned_in_smp;
};

struct nvmet_pq_thread_data {
	struct wait_queue_head wait_head;
	struct nvmet_pq_ring *ring;
	u32  ring_mem_size;
	struct list_head  submit_list;
	u32  thread_idle;
	int  affinity_cpu;
	unsigned long state;
	pid_t			task_pid;
	pid_t			task_tgid;
	struct task_struct *thread;
	struct mutex	   lock;
	struct delayed_work compl_bio;
};

struct nvmet_pq_thread_data nvmet_pqt_data;

static inline int
nvmet_pq_powerof2_enabled(u32 n)
{
	return n && !(n & (n - 1));
}

static inline u32 nvmet_pq_alignpow2(u32 x)
{
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	return x + 1;
}

static void nvmet_pq_mem_free(void *ptr, size_t size)
{
	struct page *page;

	if (!ptr)
		return;

	page = virt_to_page(ptr);
	__free_pages(page, get_order(size));
}

static void *nvmet_pq_mem_alloc(size_t size)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN;
	if (get_order(size) > MAX_ORDER)
		return NULL;
	return (void *) __get_free_pages(gfp_flags, get_order(size));
}

static struct nvmet_pq_ring *nvmet_create_pq_ring(u32 cnt)
{
	struct nvmet_pq_ring *pq_ring;
	u32 ring_size = 0, qe_size = cnt;

	if (!nvmet_pq_powerof2_enabled(cnt))
		qe_size = nvmet_pq_alignpow2(cnt + 1);
	ring_size += sizeof(struct nvmet_pq_ring);
	ring_size += qe_size * sizeof(void **);
	pq_ring = nvmet_pq_mem_alloc(ring_size);
	if (likely(pq_ring)) {
		pq_ring->cons.head = 0;
		pq_ring->cons.tail = 0;
		pq_ring->prod.head = 0;
		pq_ring->prod.tail = 0;
		pq_ring->size = qe_size;
		pq_ring->mask = qe_size - 1;
		pq_ring->capacity = nvmet_pq_powerof2_enabled(cnt)?cnt:(qe_size - 1);
	}
	return pq_ring;
}

//below is derived from FreeBSD's bufring.h
/* the actual enqueue of pointers on the ring.
 * Placed here since identical code needed in both
 * single and multi producer enqueue functions */
#define ENQUEUE_PTRS(r, ring_start, prod_head, obj_table, n, obj_type) do { \
	unsigned int i; \
	const u32 size = (r)->size; \
	u32 idx = prod_head & (r)->mask; \
	obj_type *ring = (obj_type *)ring_start; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & ((~(unsigned)0x3))); i += 4, idx += 4) { \
			ring[idx] = obj_table[i]; \
			ring[idx+1] = obj_table[i+1]; \
			ring[idx+2] = obj_table[i+2]; \
			ring[idx+3] = obj_table[i+3]; \
		} \
		switch (n & 0x3) { \
		case 3: \
			ring[idx++] = obj_table[i++]; /* fallthrough */ \
		case 2: \
			ring[idx++] = obj_table[i++]; /* fallthrough */ \
		case 1: \
			ring[idx++] = obj_table[i++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++)\
			ring[idx] = obj_table[i]; \
		for (idx = 0; i < n; i++, idx++) \
			ring[idx] = obj_table[i]; \
	} \
} while (0)

/* the actual copy of pointers on the ring to obj_table.
 * Placed here since identical code needed in both
 * single and multi consumer dequeue functions */
#define DEQUEUE_PTRS(r, ring_start, cons_head, obj_table, n, obj_type) do { \
	unsigned int i; \
	u32 idx = cons_head & (r)->mask; \
	const u32 size = (r)->size; \
	obj_type *ring = (obj_type *)ring_start; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & (~(unsigned)0x3)); i += 4, idx += 4) {\
			obj_table[i] = ring[idx]; \
			obj_table[i+1] = ring[idx+1]; \
			obj_table[i+2] = ring[idx+2]; \
			obj_table[i+3] = ring[idx+3]; \
		} \
		switch (n & 0x3) { \
		case 3: \
			obj_table[i++] = ring[idx++]; /* fallthrough */ \
		case 2: \
			obj_table[i++] = ring[idx++]; /* fallthrough */ \
		case 1: \
			obj_table[i++] = ring[idx++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++) \
			obj_table[i] = ring[idx]; \
		for (idx = 0; i < n; i++, idx++) \
			obj_table[i] = ring[idx]; \
	} \
} while (0)

static inline u32
__nvmet_pq_ring_move_prod_head(struct nvmet_pq_ring *r, u32 n,
								u32 *old_head, u32 *new_head, u32 *free_entries)
{
	const u32 capacity = smp_load_acquire(&r->capacity);
	u32 ret, success;

	do {
		*old_head = smp_load_acquire(&r->prod.head);

		/* add rmb barrier to avoid load/load reorder in weak
		 * memory model.
		 */
		smp_rmb();

		*free_entries = (capacity + smp_load_acquire(&r->cons.tail) - *old_head);

		/* check that we have enough room in ring */
		if (unlikely(n > *free_entries))
			return 0;

		*new_head = *old_head + n;
		ret = cmpxchg(&r->prod.head, *old_head, *new_head);
		success = (ret == *old_head) ? 1 : 0;
	} while (unlikely(success == 0));
	return n;
}

static inline u32
__nvmet_pq_ring_move_cons_head(struct nvmet_pq_ring *r, u32 n,
									u32 *old_head, u32 *new_head, u32 *entries)
{
	unsigned int ret, success;

	/* move cons.head atomically */
	do {
		*old_head = smp_load_acquire(&r->cons.head);

		/* add rmb barrier to avoid load/load reorder in weak
		 * memory model.
		 */
		smp_rmb();

		*entries = (smp_load_acquire(&r->prod.tail) - *old_head);

		/* check if we have enough entry to dequeue */
		if (n > *entries)
			return 0;

		*new_head = *old_head + n;
		ret = cmpxchg(&r->cons.head, *old_head, *new_head);
		success = (ret == *old_head) ? 1 : 0;
	} while (unlikely(success == 0));
	return n;
}

static inline void
__nvmet_pq_ring_update_tail(struct nvmet_pq_ring_headtail *ht,
										u32 old_val, u32 new_val, u32 enqueue)
{
	if (enqueue)
		smp_wmb();
	else
		smp_rmb();

	while (unlikely(smp_load_acquire(&ht->tail) != old_val))
		;

	smp_store_release(&ht->tail, new_val);
}

static inline u32
__nvmet_pq_ring_do_enqueue(struct nvmet_pq_ring *r,
							void **obj_table, u32 n, u32 *free_space)
{
	uint32_t prod_head, prod_next;
	uint32_t free_entries;

	n = __nvmet_pq_ring_move_prod_head(r, n, &prod_head,
									&prod_next, &free_entries);
	if (n == 0)
		goto end;

	ENQUEUE_PTRS(r, &r[1], prod_head, obj_table, n, void *);

	__nvmet_pq_ring_update_tail(&r->prod, prod_head, prod_next, 1);
end:
	if (free_space != NULL)
		*free_space = free_entries - n;
	return n;
}

static inline u32
__nvmet_pq_ring_do_dequeue(struct nvmet_pq_ring *r,
							void **obj_table, u32 n, u32 *available)
{
	uint32_t cons_head, cons_next;
	uint32_t entries;

	n = __nvmet_pq_ring_move_cons_head(r, n, &cons_head, &cons_next, &entries);
	if (n == 0)
		goto end;

	DEQUEUE_PTRS(r, &r[1], cons_head, obj_table, n, void *);

	__nvmet_pq_ring_update_tail(&r->cons, cons_head, cons_next, 0);

end:
	if (available != NULL)
		*available = entries - n;
	return n;
}

static inline u32
nvmet_pq_ring_enqueue_bulk(struct nvmet_pq_ring *r,
							void **obj_table, u32 n, u32 *free_space)
{
	return __nvmet_pq_ring_do_enqueue(r, obj_table, n, free_space);
}

static inline int
nvmet_pq_ring_enqueue(struct nvmet_pq_ring *r, void **obj)
{
	return nvmet_pq_ring_enqueue_bulk(r, obj, 1, NULL) ? 0 : -ENOBUFS;
}

static inline u32
nvmet_pq_ring_dequeue_bulk(struct nvmet_pq_ring *r,
							void **obj_table, u32 n, u32 *available)
{
	return __nvmet_pq_ring_do_dequeue(r, obj_table, n, available);
}

static inline int
nvmet_pq_ring_dequeue(struct nvmet_pq_ring *r, void **obj_p)
{
	return nvmet_pq_ring_dequeue_bulk(r, obj_p, 1, NULL) ? 0 : -ENOENT;
}

static inline u32
__nvmet_pq_ring_count(const struct nvmet_pq_ring *r)
{
	u32 prod_tail = smp_load_acquire(&r->prod.tail);
	u32 cons_tail = smp_load_acquire(&r->cons.tail);
	u32 count = (prod_tail - cons_tail) & r->mask;
	u32 capacity = smp_load_acquire(&r->capacity);
	return (count > capacity) ? capacity : count;
}

/**
 * Return the number of free entries in a ring.
 */
static inline u32
__nvmet_pq_ring_free_count(const struct nvmet_pq_ring *r)
{
	return smp_load_acquire(&r->capacity) - __nvmet_pq_ring_count(r);
}

/**
 * Test if a ring is full.
 */
static inline int
__nvmet_pq_ring_full(const struct nvmet_pq_ring *r)
{
	return __nvmet_pq_ring_free_count(r) == 0;
}

/**
 * Test if a ring is empty.
 */
static inline int
__nvmet_pq_ring_empty(const struct nvmet_pq_ring *r)
{
	return __nvmet_pq_ring_count(r) == 0;
}

/**
 * Return the size of the ring.
 */
static inline u32
__nvmet_pq_ring_get_size(const struct nvmet_pq_ring *r)
{
	return smp_load_acquire(&r->size);
}

/**
 * Return the number of elements which can be stored in the ring.
 */
static inline u32
__nvmet_pq_ring_get_capacity(const struct nvmet_pq_ring *r)
{
	return smp_load_acquire(&r->capacity);
}

#define  NVMET_PQT_IO_BUDGET  64

static inline void
__nvmet_pqt_submit_bio(struct nvmet_pq_thread_data *pqtd,
						struct nvmet_pqt_bio_req *pqt_req, bool cancel_thread)
{
	struct bio *bio;
	struct nvmet_pqt_sqe *sqe;

	while ((bio = bio_list_pop(&pqt_req->blist))) {
		if (cancel_thread) {
			nvmet_req_bio_put(pqt_req->req, bio);
			if (!bio_flagged(bio, BIO_CHAIN)) {
				kfree(pqt_req);
				break;
			}
		} else {
			if (bio_flagged(bio, BIO_CHAIN))
				submit_bio(bio);
			else {
				sqe = kmalloc(sizeof(struct nvmet_pqt_sqe), GFP_KERNEL);
				if (!sqe) {
					bio_io_error(bio);
					kfree(pqt_req);
				} else {
					sqe->bio = bio;
					sqe->pqt_req = pqt_req;
					submit_bio(bio);
					list_add(&sqe->list, &pqtd->submit_list);
				}
			}
		}
	}
}

static int __nvmet_pq_thread(struct nvmet_pq_thread_data *pqtd,
										bool cancel_thread)
{
	int ret = 0, poll_cnt = 0;
	struct nvmet_pqt_bio_req *req_done;
	struct nvmet_pqt_sqe *sqe, *tmp;
	unsigned int poll_flags = BLK_POLL_ONESHOT;
	DEFINE_IO_COMP_BATCH(iob);

	while (1) {
		ret = nvmet_pq_ring_dequeue(pqtd->ring, (void **)&req_done);
		if (ret)
			break;
		__nvmet_pqt_submit_bio(pqtd, req_done, cancel_thread);
		poll_cnt++;
		if (poll_cnt == NVMET_PQT_IO_BUDGET && !cancel_thread)
			break;
	}
	if (!list_empty(&pqtd->submit_list)) {
cancel_thread_poll_again:
		list_for_each_entry_safe(sqe, tmp, &pqtd->submit_list, list) {
			if (sqe->pqt_req->io_completed == 1) {
				list_del(&sqe->list);
				kfree(sqe->pqt_req);
				kfree(sqe);
				continue;
			}
			ret = bio_poll(sqe->bio, &iob, poll_flags);
			if (ret < 0) {
				if (!cancel_thread) {
					if (!rq_list_empty(iob.req_list))
						iob.complete(&iob);
					return 1;
				}
			}
			if (ret > 0 && sqe->pqt_req->io_completed == 1) {
				list_del(&sqe->list);
				kfree(sqe->pqt_req);
				kfree(sqe);
			}
		}
	}
	if (cancel_thread) {
		if (!list_empty(&pqtd->submit_list))
			goto cancel_thread_poll_again;
		nvmet_pq_mem_free(pqtd->ring, pqtd->ring_mem_size);
	}
	if (!rq_list_empty(iob.req_list))
		iob.complete(&iob);
	return 0;
}

void nvmet_wakeup_pq_thread(void)
{
	smp_mb();
	if (waitqueue_active(&nvmet_pqt_data.wait_head))
		wake_up(&nvmet_pqt_data.wait_head);
}

int nvmet_pqt_ring_enqueue(void *pt)
{
	struct nvmet_pqt_bio_req *pqt_req = pt;
	return nvmet_pq_ring_enqueue(nvmet_pqt_data.ring, (void **)&pqt_req);
}

static int nvmet_pq_thread(void *data)
{
	struct nvmet_pq_thread_data *pqtd = data;
	unsigned long timeout = 0;
	DEFINE_WAIT(wait);

	if (pqtd->affinity_cpu != -1)
		set_cpus_allowed_ptr(current, cpumask_of(pqtd->affinity_cpu));
	else
		set_cpus_allowed_ptr(current, cpu_online_mask);
	current->flags |= PF_NO_SETAFFINITY;
	mutex_lock(&pqtd->lock);
	pqtd->task_pid = current->pid;
	pqtd->task_tgid = current->tgid;

	while (!kthread_should_stop()) {
		if (test_bit(NVMET_PQ_THREAD_SHOULD_STOP, &pqtd->state))
			break;

		int ret = __nvmet_pq_thread(pqtd, false);
		if (ret > 0 || !time_after(jiffies, timeout)) {
			cond_resched();
			if (ret > 0)
				timeout = jiffies + pqtd->thread_idle;
			continue;
		}
		prepare_to_wait(&pqtd->wait_head, &wait, TASK_INTERRUPTIBLE);
		mutex_unlock(&pqtd->lock);
		schedule();
		mutex_lock(&pqtd->lock);
		finish_wait(&pqtd->wait_head, &wait);
		timeout = jiffies + pqtd->thread_idle;
	}
	pqtd->thread = NULL;
	pqtd->task_pid = -1;
	pqtd->task_tgid = -1;
	mutex_unlock(&pqtd->lock);
	kthread_complete_and_exit(NULL, 0);
}

bool nvmet_pqt_enabled(void)
{
	if (!test_bit(NVMET_PQ_THREAD_SHOULD_STOP, &nvmet_pqt_data.state) &&
		test_bit(NVMET_PQ_THREAD_ENABLE, &nvmet_pqt_data.state))
		return true;
	else
		return false;
}

static void nvmet_pqt_compl_bio_req_func(struct work_struct *work)
{
	struct nvmet_pq_thread_data *pqtd = container_of(work,
							struct nvmet_pq_thread_data, compl_bio.work);
	__nvmet_pq_thread(pqtd, true);
}

int nvmet_init_pq_thread(u32 thread_idle, int affinity_cpu, u32 ring_size)
{
	struct task_struct *task;
	int ret = 0;

	memset(&nvmet_pqt_data, 0, sizeof(struct nvmet_pq_thread_data));
	init_waitqueue_head(&nvmet_pqt_data.wait_head);
	mutex_init(&nvmet_pqt_data.lock);
	nvmet_pqt_data.thread_idle = usecs_to_jiffies(thread_idle);
	nvmet_pqt_data.affinity_cpu = affinity_cpu;
	INIT_LIST_HEAD(&nvmet_pqt_data.submit_list);
	nvmet_pqt_data.ring = nvmet_create_pq_ring(ring_size);
	if (!nvmet_pqt_data.ring) {
		printk(KERN_ERR "allocate poll ring failure\n");
		return -1;
	}
	nvmet_pqt_data.ring_mem_size = sizeof(struct nvmet_pq_ring);
	nvmet_pqt_data.ring_mem_size += nvmet_pqt_data.ring->size * sizeof(void **);
	task = kthread_create(nvmet_pq_thread, (void *)&nvmet_pqt_data, "nvmet-pqt");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto err;
	}

	set_user_nice(task, -20);
	mutex_lock(&nvmet_pqt_data.lock);
	nvmet_pqt_data.thread = task;
	mutex_unlock(&nvmet_pqt_data.lock);
	wake_up_process(task);
	set_bit(NVMET_PQ_THREAD_ENABLE, &nvmet_pqt_data.state);
	return 0;
err:
	nvmet_pq_mem_free(nvmet_pqt_data.ring, nvmet_pqt_data.ring_mem_size);
	return ret;
}

void nvmet_exit_pq_thread(void)
{
	set_bit(NVMET_PQ_THREAD_SHOULD_STOP, &nvmet_pqt_data.state);
	clear_bit(NVMET_PQ_THREAD_ENABLE, &nvmet_pqt_data.state);
	mutex_lock(&nvmet_pqt_data.lock);
	if (nvmet_pqt_data.thread) {
		mutex_unlock(&nvmet_pqt_data.lock);
		kthread_stop(nvmet_pqt_data.thread);
	} else {
		mutex_unlock(&nvmet_pqt_data.lock);
	}
	INIT_DELAYED_WORK(&nvmet_pqt_data.compl_bio, nvmet_pqt_compl_bio_req_func);
	schedule_delayed_work(&nvmet_pqt_data.compl_bio, 3);
}
