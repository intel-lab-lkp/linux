// SPDX-License-Identifier: GPL-2.0

/*
 * Test module for page_pool
 *
 * Copyright (C) 2024 Yunsheng Lin <linyunsheng@huawei.com>
 */

#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/ptr_ring.h>
#include <linux/kthread.h>
#include <net/page_pool/helpers.h>

static struct ptr_ring ptr_ring;
static int nr_objs = 512;
static atomic_t nthreads;
static struct completion wait;
static struct page_pool *test_pool;
static struct device *dev;
static u64 dma_mask = DMA_BIT_MASK(64);

static int nr_test = 2000000;
module_param(nr_test, int, 0);
MODULE_PARM_DESC(nr_test, "number of iterations to test");

static bool test_frag;
module_param(test_frag, bool, 0);
MODULE_PARM_DESC(test_frag, "use frag API for testing");

static bool test_dma;
module_param(test_dma, bool, 0);
MODULE_PARM_DESC(test_dma, "enable dma mapping for testing");

static bool test_napi;
module_param(test_napi, bool, 0);
MODULE_PARM_DESC(test_napi, "use NAPI softirq for testing");

static bool test_direct;
module_param(test_direct, bool, 0);
MODULE_PARM_DESC(test_direct, "enable direct recycle for testing");

static int test_alloc_len = 2048;
module_param(test_alloc_len, int, 0);
MODULE_PARM_DESC(test_alloc_len, "alloc len for testing");

static int test_push_cpu;
module_param(test_push_cpu, int, 0);
MODULE_PARM_DESC(test_push_cpu, "test cpu for pushing page");

static int test_pop_cpu;
module_param(test_pop_cpu, int, 0);
MODULE_PARM_DESC(test_pop_cpu, "test cpu for popping page");

static void page_pool_test_dev_release(struct device *dev)
{
	kfree(dev);
}

static struct page_pool *page_pool_test_create(void)
{
	struct page_pool_params page_pool_params = {
		.pool_size = nr_objs,
		.flags = 0,
		.nid = cpu_to_mem(test_push_cpu),
	};
	int ret;

	if (test_dma) {
		dev = kzalloc(sizeof(*dev), GFP_KERNEL);
		if (!dev)
			return ERR_PTR(-ENOMEM);

		dev->release = page_pool_test_dev_release;
		dev->dma_mask = &dma_mask;
		device_initialize(dev);

		ret = dev_set_name(dev, "page_pool_dev");
		if (ret) {
			pr_err("page_pool_test dev_set_name() failed: %d\n",
			       ret);
			goto err_out;
		}

		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (ret) {
			pr_err("page_pool_test set dma mask failed: %d\n",
			       ret);
			goto err_out;
		}

		ret = device_add(dev);
		if (ret) {
			pr_err("page_pool_test device_add() failed: %d\n", ret);
			goto err_out;
		}

		page_pool_params.dev = dev;
		page_pool_params.flags |= PP_FLAG_DMA_MAP;
		page_pool_params.dma_dir = DMA_FROM_DEVICE;
	}

	return page_pool_create(&page_pool_params);
err_out:
	put_device(dev);
	return ERR_PTR(ret);
}

static void page_pool_test_destroy(struct page_pool *pool)
{
	page_pool_destroy(pool);

	if (test_dma) {
		device_del(dev);
		put_device(dev);
	}
}

static int test_pushed;
static int test_popped;
static int page_pool_pop_thread(void *arg)
{
	struct ptr_ring *ring = arg;

	pr_info("page_pool pop test thread begins on cpu %d\n",
		smp_processor_id());

	while (test_popped < nr_test) {
		void *obj = __ptr_ring_consume(ring);

		if (obj) {
			test_popped++;
			page_pool_put_full_page(test_pool, obj, false);
		} else {
			cond_resched();
		}
	}

	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	pr_info("page_pool pop test thread exits on cpu %d\n",
		smp_processor_id());

	return 0;
}

static int page_pool_push_thread(void *arg)
{
	struct ptr_ring *ring = arg;

	pr_info("page_pool push test thread begins on cpu %d\n",
		smp_processor_id());

	while (test_pushed < nr_test) {
		struct page *page;
		int ret;

		if (test_frag) {
			unsigned int offset;

			page = page_pool_dev_alloc_frag(test_pool, &offset,
							test_alloc_len);
		} else {
			page = page_pool_dev_alloc_pages(test_pool);
		}

		if (!page)
			continue;

		ret = __ptr_ring_produce(ring, page);
		if (ret) {
			page_pool_put_full_page(test_pool, page, true);
			cond_resched();
		} else {
			test_pushed++;
		}
	}

	pr_info("page_pool push test thread exits on cpu %d\n",
		smp_processor_id());

	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	return 0;
}

static int page_pool_push_poll(struct napi_struct *napi, int budget)
{
	static bool print = true;
	int processed = 0;

	if (unlikely(print)) {
		pr_info("page_pool push test napi begins on cpu %d\n",
			smp_processor_id());
		print = false;
	}

	while (processed < budget && test_pushed < nr_test) {
		struct page *page;
		int ret;

		if (test_frag) {
			unsigned int offset;

			page = page_pool_dev_alloc_frag(test_pool, &offset,
							test_alloc_len);
		} else {
			page = page_pool_dev_alloc_pages(test_pool);
		}

		if (!page)
			return budget;

		ret = __ptr_ring_produce(&ptr_ring, page);
		if (ret) {
			page_pool_put_full_page(test_pool, page, true);
			return budget;
		}

		processed++;
		test_pushed++;
	}

	if (test_pushed < nr_test)
		return budget;

	pr_info("page_pool push test napi exits on cpu %d\n",
		smp_processor_id());

	napi_complete(napi);
	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	return 0;
}

static int page_pool_pop_poll(struct napi_struct *napi, int budget)
{
	static bool print = true;
	int processed = 0;

	if (unlikely(print)) {
		pr_info("page_pool pop test napi begins on cpu %d\n",
			smp_processor_id());
		print = false;
	}

	while (processed < budget && test_popped < nr_test) {
		void *obj = __ptr_ring_consume(&ptr_ring);

		if (obj) {
			processed++;
			test_popped++;
			page_pool_put_full_page(test_pool, obj, test_direct);
		} else {
			return budget;
		}
	}

	if (test_popped < nr_test)
		return budget;

	if (atomic_dec_and_test(&nthreads))
		complete(&wait);

	napi_complete(napi);
	pr_info("page_pool pop test napi exits on cpu %d\n",
		smp_processor_id());

	return 0;
}

static int page_pool_create_test_thread(void)
{
	struct task_struct *tsk_push, *tsk_pop;

	tsk_push = kthread_create_on_cpu(page_pool_push_thread, &ptr_ring,
					 test_push_cpu, "page_pool_push");
	if (IS_ERR(tsk_push))
		return PTR_ERR(tsk_push);

	tsk_pop = kthread_create_on_cpu(page_pool_pop_thread, &ptr_ring,
					test_pop_cpu, "page_pool_pop");
	if (IS_ERR(tsk_pop)) {
		kthread_stop(tsk_push);
		return PTR_ERR(tsk_pop);
	}

	wake_up_process(tsk_push);
	wake_up_process(tsk_pop);

	return 0;
}

static struct napi_struct *pop_napi, *push_napi;
static struct net_device *netdev;
static int page_pool_schedule_napi(void *arg)
{
	struct napi_struct *napi = arg;

	napi_schedule_irqoff(napi);

	return 0;
}

static int page_pool_create_test_napi(void)
{
	struct task_struct *push_tsk, *pop_tsk;
	int ret;

	netdev = alloc_etherdev(sizeof(struct napi_struct) * 2);
	if (!netdev)
		return -ENOMEM;

	pop_napi = netdev_priv(netdev);
	push_napi = pop_napi + 1;

	netif_napi_add(netdev, push_napi, page_pool_push_poll);
	netif_napi_add(netdev, pop_napi, page_pool_pop_poll);

	napi_enable(push_napi);
	napi_enable(pop_napi);

	push_tsk = kthread_create_on_cpu(page_pool_schedule_napi, push_napi,
					 test_push_cpu, "page_pool_push_napi");
	if (IS_ERR(push_tsk)) {
		ret = PTR_ERR(push_tsk);
		goto err_alloc_etherdev;
	}

	pop_tsk = kthread_create_on_cpu(page_pool_schedule_napi, pop_napi,
					test_pop_cpu, "page_pool_pop_napi");
	if (IS_ERR(pop_tsk)) {
		ret = PTR_ERR(pop_tsk);
		goto err_push_thread;
	}

	wake_up_process(push_tsk);
	wake_up_process(pop_tsk);
	return 0;

err_push_thread:
	kthread_stop(push_tsk);
err_alloc_etherdev:
	free_netdev(netdev);
	return ret;
}

static void page_pool_destroy_test_napi(void)
{
	napi_disable(push_napi);
	napi_disable(pop_napi);

	netif_napi_del(push_napi);
	netif_napi_del(pop_napi);

	free_netdev(netdev);
}

static int __init page_pool_test_init(void)
{
	ktime_t start;
	u64 duration;
	int ret;

	if (test_alloc_len > PAGE_SIZE || test_alloc_len <= 0 ||
	    !cpu_active(test_push_cpu) || !cpu_active(test_pop_cpu) ||
	    (test_direct && (test_push_cpu != test_pop_cpu || !test_napi)))
		return -EINVAL;

	ret = ptr_ring_init(&ptr_ring, nr_objs, GFP_KERNEL);
	if (ret)
		return ret;

	test_pool = page_pool_test_create();
	if (IS_ERR(test_pool)) {
		ret = PTR_ERR(test_pool);
		goto err_ptr_ring_init;
	}

	atomic_set(&nthreads, 2);
	init_completion(&wait);

	if (test_napi)
		ret = page_pool_create_test_napi();
	else
		ret = page_pool_create_test_thread();
	if (ret)
		goto err_pool_create;

	start = ktime_get();
	pr_info("waiting for test to complete\n");

	while (!wait_for_completion_timeout(&wait, msecs_to_jiffies(20000)))
		pr_info("page_pool_test progress: pushed = %d, popped = %d\n",
			test_pushed, test_popped);

	duration = (u64)ktime_us_delta(ktime_get(), start);
	pr_info("%d of iterations for %s%s%s%s testing took: %lluus\n",
		nr_test, test_napi ? "napi" : "thread",
		test_direct ? " direct" : "", test_dma ? " dma" : "",
		test_frag ? " frag" : "", duration);

	ptr_ring_cleanup(&ptr_ring, NULL);
	page_pool_test_destroy(test_pool);

	if (test_napi)
		page_pool_destroy_test_napi();

	return -EAGAIN;

err_pool_create:
	page_pool_test_destroy(test_pool);
err_ptr_ring_init:
	ptr_ring_cleanup(&ptr_ring, NULL);
	return ret;
}

static void __exit page_pool_test_exit(void)
{
}

module_init(page_pool_test_init);
module_exit(page_pool_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yunsheng Lin <linyunsheng@huawei.com>");
MODULE_DESCRIPTION("Test module for page_pool");
