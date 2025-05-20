// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 HiSilicon Limited.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/map_benchmark.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/iommu-dma.h>

struct map_benchmark_data {
	struct map_benchmark bparam;
	struct device *dev;
	struct dentry  *debugfs;
	enum dma_data_direction dir;
	atomic64_t sum_map_100ns;
	atomic64_t sum_unmap_100ns;
	atomic64_t sum_sq_map;
	atomic64_t sum_sq_unmap;
	atomic64_t loops;

	/* IOVA-specific counters */
	atomic64_t sum_iova_alloc_100ns;
	atomic64_t sum_iova_link_100ns;
	atomic64_t sum_iova_sync_100ns;
	atomic64_t sum_iova_destroy_100ns;
	atomic64_t sum_sq_iova_alloc;
	atomic64_t sum_sq_iova_link;
	atomic64_t sum_sq_iova_sync;
	atomic64_t sum_sq_iova_destroy;
	atomic64_t iova_loops;
};

static int benchmark_thread_iova(void *data)
{
	void *buf;
	struct map_benchmark_data *map = data;
	int npages = map->bparam.granule;
	u64 size = npages * PAGE_SIZE;
	int ret = 0;
	enum dma_data_direction dir = map->dir;

	buf = alloc_pages_exact(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		struct dma_iova_state iova_state;
		phys_addr_t phys;
		ktime_t alloc_stime, alloc_etime, link_stime, link_etime;
		ktime_t sync_stime, sync_etime, destroy_stime, destroy_etime;
		ktime_t alloc_delta, link_delta, sync_delta, destroy_delta;
		u64 alloc_100ns, link_100ns, sync_100ns, destroy_100ns;
		u64 alloc_sq, link_sq, sync_sq, destroy_sq;

		/* Stain cache if needed */
		if (map->dir != DMA_FROM_DEVICE)
			memset(buf, 0x66, size);

		phys = virt_to_phys(buf);

		/* IOVA allocation */
		alloc_stime = ktime_get();
		if (!dma_iova_try_alloc(map->dev, &iova_state, phys, size)) {
			pr_warn_once("IOVA allocation not supported on device %s\n",
				     dev_name(map->dev));
			/* IOVA not supported, skip this iteration */
			cond_resched();
			continue;
		}
		alloc_etime = ktime_get();
		alloc_delta = ktime_sub(alloc_etime, alloc_stime);

		/* IOVA linking */
		link_stime = ktime_get();
		ret = dma_iova_link(map->dev, &iova_state, phys, 0, size, dir, 0);
		link_etime = ktime_get();
		link_delta = ktime_sub(link_etime, link_stime);

		if (ret) {
			pr_err("dma_iova_link failed on %s\n", dev_name(map->dev));
			dma_iova_free(map->dev, &iova_state);
			ret = -ENOMEM;
			goto out;
		}

		/* IOVA sync */
		sync_stime = ktime_get();
		ret = dma_iova_sync(map->dev, &iova_state, 0, size);
		sync_etime = ktime_get();
		sync_delta = ktime_sub(sync_etime, sync_stime);

		if (ret) {
			pr_err("dma_iova_sync failed on %s\n", dev_name(map->dev));
			dma_iova_unlink(map->dev, &iova_state, 0, size, dir, 0);
			dma_iova_free(map->dev, &iova_state);
			ret = -ENOMEM;
			goto out;
		}

		/* Pretend DMA is transmitting */
		ndelay(map->bparam.dma_trans_ns);

		/* IOVA destroy */
		destroy_stime = ktime_get();
		dma_iova_destroy(map->dev, &iova_state, size, dir, 0);
		destroy_etime = ktime_get();
		destroy_delta = ktime_sub(destroy_etime, destroy_stime);

		/* Calculate sum and sum of squares */
		alloc_100ns = div64_ul(alloc_delta, 100);
		link_100ns = div64_ul(link_delta, 100);
		sync_100ns = div64_ul(sync_delta, 100);
		destroy_100ns = div64_ul(destroy_delta, 100);

		alloc_sq = alloc_100ns * alloc_100ns;
		link_sq = link_100ns * link_100ns;
		sync_sq = sync_100ns * sync_100ns;
		destroy_sq = destroy_100ns * destroy_100ns;

		atomic64_add(alloc_100ns, &map->sum_iova_alloc_100ns);
		atomic64_add(link_100ns, &map->sum_iova_link_100ns);
		atomic64_add(sync_100ns, &map->sum_iova_sync_100ns);
		atomic64_add(destroy_100ns, &map->sum_iova_destroy_100ns);

		atomic64_add(alloc_sq, &map->sum_sq_iova_alloc);
		atomic64_add(link_sq, &map->sum_sq_iova_link);
		atomic64_add(sync_sq, &map->sum_sq_iova_sync);
		atomic64_add(destroy_sq, &map->sum_sq_iova_destroy);

		atomic64_inc(&map->iova_loops);

		cond_resched();
	}

out:
	free_pages_exact(buf, size);
	return ret;
}

static int benchmark_thread_streaming(void *data)
{
	void *buf;
	dma_addr_t dma_addr;
	struct map_benchmark_data *map = data;
	int npages = map->bparam.granule;
	u64 size = npages * PAGE_SIZE;
	int ret = 0;

	buf = alloc_pages_exact(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!kthread_should_stop())  {
		u64 map_100ns, unmap_100ns, map_sq, unmap_sq;
		ktime_t map_stime, map_etime, unmap_stime, unmap_etime;
		ktime_t map_delta, unmap_delta;

		/*
		 * for a non-coherent device, if we don't stain them in the
		 * cache, this will give an underestimate of the real-world
		 * overhead of BIDIRECTIONAL or TO_DEVICE mappings;
		 * 66 means evertything goes well! 66 is lucky.
		 */
		if (map->dir != DMA_FROM_DEVICE)
			memset(buf, 0x66, size);

		map_stime = ktime_get();
		dma_addr = dma_map_single(map->dev, buf, size, map->dir);
		if (unlikely(dma_mapping_error(map->dev, dma_addr))) {
			pr_err("dma_map_single failed on %s\n",
				dev_name(map->dev));
			ret = -ENOMEM;
			goto out;
		}
		map_etime = ktime_get();
		map_delta = ktime_sub(map_etime, map_stime);

		/* Pretend DMA is transmitting */
		ndelay(map->bparam.dma_trans_ns);

		unmap_stime = ktime_get();
		dma_unmap_single(map->dev, dma_addr, size, map->dir);
		unmap_etime = ktime_get();
		unmap_delta = ktime_sub(unmap_etime, unmap_stime);

		/* calculate sum and sum of squares */

		map_100ns = div64_ul(map_delta,  100);
		unmap_100ns = div64_ul(unmap_delta, 100);
		map_sq = map_100ns * map_100ns;
		unmap_sq = unmap_100ns * unmap_100ns;

		atomic64_add(map_100ns, &map->sum_map_100ns);
		atomic64_add(unmap_100ns, &map->sum_unmap_100ns);
		atomic64_add(map_sq, &map->sum_sq_map);
		atomic64_add(unmap_sq, &map->sum_sq_unmap);
		atomic64_inc(&map->loops);

		/*
		 * We may test for a long time so periodically check whether
		 * we need to schedule to avoid starving the others. Otherwise
		 * we may hangup the kernel in a non-preemptible kernel when
		 * the test kthreads number >= CPU number, the test kthreads
		 * will run endless on every CPU since the thread resposible
		 * for notifying the kthread stop (in do_streaming_benchmark())
		 * could not be scheduled.
		 *
		 * Note this may degrade the test concurrency since the test
		 * threads may need to share the CPU time with other load
		 * in the system. So it's recommended to run this benchmark
		 * on an idle system.
		 */
		cond_resched();
	}

out:
	free_pages_exact(buf, size);
	return ret;
}

static int do_iova_benchmark(struct map_benchmark_data *map)
{
	struct task_struct **tsk;
	int threads = map->bparam.threads;
	int node = map->bparam.node;
	u64 iova_loops;
	int ret = 0;
	int i;

	tsk = kmalloc_array(threads, sizeof(*tsk), GFP_KERNEL);
	if (!tsk)
		return -ENOMEM;

	get_device(map->dev);

	/* Create IOVA threads only */
	for (i = 0; i < threads; i++) {
		tsk[i] = kthread_create_on_node(benchmark_thread_iova, map,
				node, "dma-iova-benchmark/%d", i);
		if (IS_ERR(tsk[i])) {
			pr_err("create dma_iova thread failed\n");
			ret = PTR_ERR(tsk[i]);
			while (--i >= 0)
				kthread_stop(tsk[i]);
			goto out;
		}

		if (node != NUMA_NO_NODE)
			kthread_bind_mask(tsk[i], cpumask_of_node(node));
	}

	/* Clear previous IOVA benchmark values */
	atomic64_set(&map->sum_iova_alloc_100ns, 0);
	atomic64_set(&map->sum_iova_link_100ns, 0);
	atomic64_set(&map->sum_iova_sync_100ns, 0);
	atomic64_set(&map->sum_iova_destroy_100ns, 0);
	atomic64_set(&map->sum_sq_iova_alloc, 0);
	atomic64_set(&map->sum_sq_iova_link, 0);
	atomic64_set(&map->sum_sq_iova_sync, 0);
	atomic64_set(&map->sum_sq_iova_destroy, 0);
	atomic64_set(&map->iova_loops, 0);

	/* Start all threads */
	for (i = 0; i < threads; i++) {
		get_task_struct(tsk[i]);
		wake_up_process(tsk[i]);
	}

	msleep_interruptible(map->bparam.seconds * 1000);

	/* Stop all threads */
	for (i = 0; i < threads; i++) {
		int kthread_ret = kthread_stop_put(tsk[i]);
		if (kthread_ret)
			ret = kthread_ret;
	}

	if (ret)
		goto out;

	/* Calculate IOVA statistics */
	iova_loops = atomic64_read(&map->iova_loops);
	if (likely(iova_loops > 0)) {
		u64 alloc_variance, link_variance, sync_variance, destroy_variance;
		u64 sum_alloc = atomic64_read(&map->sum_iova_alloc_100ns);
		u64 sum_link = atomic64_read(&map->sum_iova_link_100ns);
		u64 sum_sync = atomic64_read(&map->sum_iova_sync_100ns);
		u64 sum_destroy = atomic64_read(&map->sum_iova_destroy_100ns);
		u64 sum_sq_alloc = atomic64_read(&map->sum_sq_iova_alloc);
		u64 sum_sq_link = atomic64_read(&map->sum_sq_iova_link);
		u64 sum_sq_sync = atomic64_read(&map->sum_sq_iova_sync);
		u64 sum_sq_destroy = atomic64_read(&map->sum_sq_iova_destroy);

		/* Average latencies */
		map->bparam.avg_iova_alloc_100ns = div64_u64(sum_alloc, iova_loops);
		map->bparam.avg_iova_link_100ns = div64_u64(sum_link, iova_loops);
		map->bparam.avg_iova_sync_100ns = div64_u64(sum_sync, iova_loops);
		map->bparam.avg_iova_destroy_100ns = div64_u64(sum_destroy, iova_loops);

		/* Standard deviations */
		alloc_variance = div64_u64(sum_sq_alloc, iova_loops) -
				map->bparam.avg_iova_alloc_100ns * map->bparam.avg_iova_alloc_100ns;
		link_variance = div64_u64(sum_sq_link, iova_loops) -
				map->bparam.avg_iova_link_100ns * map->bparam.avg_iova_link_100ns;
		sync_variance = div64_u64(sum_sq_sync, iova_loops) -
				map->bparam.avg_iova_sync_100ns * map->bparam.avg_iova_sync_100ns;
		destroy_variance = div64_u64(sum_sq_destroy, iova_loops) -
				map->bparam.avg_iova_destroy_100ns * map->bparam.avg_iova_destroy_100ns;

		map->bparam.iova_alloc_stddev = int_sqrt64(alloc_variance);
		map->bparam.iova_link_stddev = int_sqrt64(link_variance);
		map->bparam.iova_sync_stddev = int_sqrt64(sync_variance);
		map->bparam.iova_destroy_stddev = int_sqrt64(destroy_variance);
	}

out:
	put_device(map->dev);
	kfree(tsk);
	return ret;
}

static int do_streaming_iova_benchmark(struct map_benchmark_data *map)
{
	struct task_struct **tsk;
	int threads = map->bparam.threads;
	int node = map->bparam.node;
	int regular_threads, iova_threads;
	u64 loops, iova_loops;
	int ret = 0;
	int i;

	tsk = kmalloc_array(threads * 2, sizeof(*tsk), GFP_KERNEL);
	if (!tsk)
		return -ENOMEM;

	get_device(map->dev);

	/* Split threads between regular and IOVA testing */
	regular_threads = threads / 2;
	iova_threads = threads - regular_threads;

	/* Create streaming DMA threads */
	for (i = 0; i < regular_threads; i++) {
		tsk[i] = kthread_create_on_node(benchmark_thread_streaming, map,
				node, "dma-streaming-benchmark/%d", i);
		if (IS_ERR(tsk[i])) {
			pr_err("create dma_map thread failed\n");
			ret = PTR_ERR(tsk[i]);
			while (--i >= 0)
				kthread_stop(tsk[i]);
			goto out;
		}

		if (node != NUMA_NO_NODE)
			kthread_bind_mask(tsk[i], cpumask_of_node(node));
	}

	/* Create IOVA DMA threads */
	for (i = regular_threads; i < threads; i++) {
		tsk[i] = kthread_create_on_node(benchmark_thread_iova, map,
				node, "dma-iova-benchmark/%d", i - regular_threads);
		if (IS_ERR(tsk[i])) {
			pr_err("create dma_iova thread failed\n");
			ret = PTR_ERR(tsk[i]);
			while (--i >= 0)
				kthread_stop(tsk[i]);
			goto out;
		}

		if (node != NUMA_NO_NODE)
			kthread_bind_mask(tsk[i], cpumask_of_node(node));
	}

	/* Clear previous benchmark values */
	atomic64_set(&map->sum_map_100ns, 0);
	atomic64_set(&map->sum_unmap_100ns, 0);
	atomic64_set(&map->sum_sq_map, 0);
	atomic64_set(&map->sum_sq_unmap, 0);
	atomic64_set(&map->loops, 0);

	atomic64_set(&map->sum_iova_alloc_100ns, 0);
	atomic64_set(&map->sum_iova_link_100ns, 0);
	atomic64_set(&map->sum_iova_sync_100ns, 0);
	atomic64_set(&map->sum_iova_destroy_100ns, 0);
	atomic64_set(&map->sum_sq_iova_alloc, 0);
	atomic64_set(&map->sum_sq_iova_link, 0);
	atomic64_set(&map->sum_sq_iova_sync, 0);
	atomic64_set(&map->sum_sq_iova_destroy, 0);
	atomic64_set(&map->iova_loops, 0);

	/* Start all threads */
	for (i = 0; i < threads; i++) {
		get_task_struct(tsk[i]);
		wake_up_process(tsk[i]);
	}

	msleep_interruptible(map->bparam.seconds * 1000);

	/* Stop all threads */
	for (i = 0; i < threads; i++) {
		int kthread_ret = kthread_stop_put(tsk[i]);
		if (kthread_ret)
			ret = kthread_ret;
	}

	if (ret)
		goto out;

	/* Calculate streaming DMA statistics */
	loops = atomic64_read(&map->loops);
	if (loops > 0) {
		u64 map_variance, unmap_variance;
		u64 sum_map = atomic64_read(&map->sum_map_100ns);
		u64 sum_unmap = atomic64_read(&map->sum_unmap_100ns);
		u64 sum_sq_map = atomic64_read(&map->sum_sq_map);
		u64 sum_sq_unmap = atomic64_read(&map->sum_sq_unmap);

		map->bparam.avg_map_100ns = div64_u64(sum_map, loops);
		map->bparam.avg_unmap_100ns = div64_u64(sum_unmap, loops);

		map_variance = div64_u64(sum_sq_map, loops) -
				map->bparam.avg_map_100ns * map->bparam.avg_map_100ns;
		unmap_variance = div64_u64(sum_sq_unmap, loops) -
				map->bparam.avg_unmap_100ns * map->bparam.avg_unmap_100ns;
		map->bparam.map_stddev = int_sqrt64(map_variance);
		map->bparam.unmap_stddev = int_sqrt64(unmap_variance);
	}

	/* Calculate IOVA statistics */
	iova_loops = atomic64_read(&map->iova_loops);
	if (iova_loops > 0) {
		u64 alloc_variance, link_variance, sync_variance, destroy_variance;
		u64 sum_alloc = atomic64_read(&map->sum_iova_alloc_100ns);
		u64 sum_link = atomic64_read(&map->sum_iova_link_100ns);
		u64 sum_sync = atomic64_read(&map->sum_iova_sync_100ns);
		u64 sum_destroy = atomic64_read(&map->sum_iova_destroy_100ns);

		map->bparam.avg_iova_alloc_100ns = div64_u64(sum_alloc, iova_loops);
		map->bparam.avg_iova_link_100ns = div64_u64(sum_link, iova_loops);
		map->bparam.avg_iova_sync_100ns = div64_u64(sum_sync, iova_loops);
		map->bparam.avg_iova_destroy_100ns = div64_u64(sum_destroy, iova_loops);

		alloc_variance = div64_u64(atomic64_read(&map->sum_sq_iova_alloc), iova_loops) -
				map->bparam.avg_iova_alloc_100ns * map->bparam.avg_iova_alloc_100ns;
		link_variance = div64_u64(atomic64_read(&map->sum_sq_iova_link), iova_loops) -
				map->bparam.avg_iova_link_100ns * map->bparam.avg_iova_link_100ns;
		sync_variance = div64_u64(atomic64_read(&map->sum_sq_iova_sync), iova_loops) -
				map->bparam.avg_iova_sync_100ns * map->bparam.avg_iova_sync_100ns;
		destroy_variance = div64_u64(atomic64_read(&map->sum_sq_iova_destroy), iova_loops) -
				map->bparam.avg_iova_destroy_100ns * map->bparam.avg_iova_destroy_100ns;

		map->bparam.iova_alloc_stddev = int_sqrt64(alloc_variance);
		map->bparam.iova_link_stddev = int_sqrt64(link_variance);
		map->bparam.iova_sync_stddev = int_sqrt64(sync_variance);
		map->bparam.iova_destroy_stddev = int_sqrt64(destroy_variance);
	}

out:
	put_device(map->dev);
	kfree(tsk);
	return ret;
}

static int do_streaming_benchmark(struct map_benchmark_data *map)
{
	struct task_struct **tsk;
	int threads = map->bparam.threads;
	int node = map->bparam.node;
	u64 loops;
	int ret = 0;
	int i;

	tsk = kmalloc_array(threads, sizeof(*tsk), GFP_KERNEL);
	if (!tsk)
		return -ENOMEM;

	get_device(map->dev);

	for (i = 0; i < threads; i++) {
		tsk[i] = kthread_create_on_node(benchmark_thread_streaming, map,
				map->bparam.node, "dma-streaming-benchmark/%d", i);
		if (IS_ERR(tsk[i])) {
			pr_err("create dma_map thread failed\n");
			ret = PTR_ERR(tsk[i]);
			while (--i >= 0)
				kthread_stop(tsk[i]);
			goto out;
		}

		if (node != NUMA_NO_NODE)
			kthread_bind_mask(tsk[i], cpumask_of_node(node));
	}

	/* clear the old value in the previous benchmark */
	atomic64_set(&map->sum_map_100ns, 0);
	atomic64_set(&map->sum_unmap_100ns, 0);
	atomic64_set(&map->sum_sq_map, 0);
	atomic64_set(&map->sum_sq_unmap, 0);
	atomic64_set(&map->loops, 0);

	for (i = 0; i < threads; i++) {
		get_task_struct(tsk[i]);
		wake_up_process(tsk[i]);
	}

	msleep_interruptible(map->bparam.seconds * 1000);

	/* wait for the completion of all started benchmark threads */
	for (i = 0; i < threads; i++) {
		int kthread_ret = kthread_stop_put(tsk[i]);

		if (kthread_ret)
			ret = kthread_ret;
	}

	if (ret)
		goto out;

	loops = atomic64_read(&map->loops);
	if (likely(loops > 0)) {
		u64 map_variance, unmap_variance;
		u64 sum_map = atomic64_read(&map->sum_map_100ns);
		u64 sum_unmap = atomic64_read(&map->sum_unmap_100ns);
		u64 sum_sq_map = atomic64_read(&map->sum_sq_map);
		u64 sum_sq_unmap = atomic64_read(&map->sum_sq_unmap);

		/* average latency */
		map->bparam.avg_map_100ns = div64_u64(sum_map, loops);
		map->bparam.avg_unmap_100ns = div64_u64(sum_unmap, loops);

		/* standard deviation of latency */
		map_variance = div64_u64(sum_sq_map, loops) -
				map->bparam.avg_map_100ns *
				map->bparam.avg_map_100ns;
		unmap_variance = div64_u64(sum_sq_unmap, loops) -
				map->bparam.avg_unmap_100ns *
				map->bparam.avg_unmap_100ns;
		map->bparam.map_stddev = int_sqrt64(map_variance);
		map->bparam.unmap_stddev = int_sqrt64(unmap_variance);
	}

out:
	put_device(map->dev);
	kfree(tsk);
	return ret;
}

static int validate_benchmark_params(struct map_benchmark_data *map)
{
	if (map->bparam.threads == 0 ||
	    map->bparam.threads > DMA_MAP_MAX_THREADS) {
		pr_err("invalid thread number\n");
		return -EINVAL;
	}

	if (map->bparam.seconds == 0 ||
	    map->bparam.seconds > DMA_MAP_MAX_SECONDS) {
		pr_err("invalid duration seconds\n");
		return -EINVAL;
	}

	if (map->bparam.dma_trans_ns > DMA_MAP_MAX_TRANS_DELAY) {
		pr_err("invalid transmission delay\n");
		return -EINVAL;
	}

	if (map->bparam.node != NUMA_NO_NODE &&
	    (map->bparam.node < 0 || map->bparam.node >= MAX_NUMNODES ||
	     !node_possible(map->bparam.node))) {
		pr_err("invalid numa node\n");
		return -EINVAL;
	}

	if (map->bparam.granule < 1 || map->bparam.granule > 1024) {
		pr_err("invalid granule size\n");
		return -EINVAL;
	}

	switch (map->bparam.dma_dir) {
	case DMA_MAP_BIDIRECTIONAL:
		map->dir = DMA_BIDIRECTIONAL;
		break;
	case DMA_MAP_FROM_DEVICE:
		map->dir = DMA_FROM_DEVICE;
		break;
	case DMA_MAP_TO_DEVICE:
		map->dir = DMA_TO_DEVICE;
		break;
	default:
		pr_err("invalid DMA direction\n");
		return -EINVAL;
	}

	return 0;
}

static long map_benchmark_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct map_benchmark_data *map = file->private_data;
	void __user *argp = (void __user *)arg;
	u64 old_dma_mask;
	int ret;

	if (copy_from_user(&map->bparam, argp, sizeof(map->bparam)))
		return -EFAULT;

	ret = validate_benchmark_params(map);
	if (ret)
		return ret;

	if (!use_dma_iommu(map->dev))
		map->bparam.has_iommu_dma = 0;
	else
		map->bparam.has_iommu_dma = 1;

	switch (cmd) {
	case DMA_MAP_BENCHMARK:
		old_dma_mask = dma_get_mask(map->dev);
		ret = dma_set_mask(map->dev,
				   DMA_BIT_MASK(map->bparam.dma_bits));
		if (ret) {
			pr_err("failed to set dma_mask on device %s\n",
				dev_name(map->dev));
			return -EINVAL;
		}

		/* Run streaming DMA benchmark */
		ret = do_streaming_benchmark(map);

		/*
		 * restore the original dma_mask as many devices' dma_mask are
		 * set by architectures, acpi, busses. When we bind them back
		 * to their original drivers, those drivers shouldn't see
		 * dma_mask changed by benchmark
		 */
		dma_set_mask(map->dev, old_dma_mask);

		if (ret)
			return ret;
		break;

	case DMA_MAP_BENCHMARK_IOVA:
		if (!use_dma_iommu(map->dev)) {
			pr_info("IOVA API is not supported on this device, lacks IOMMU DMA%s\n",
				dev_name(map->dev));
			return -EOPNOTSUPP;
		}
		/* Validate IOVA-specific parameters */
		if (map->bparam.use_iova > 2) {
			pr_err("invalid IOVA mode, must be 0-2\n");
			return -EINVAL;
		}

		/* Save and set DMA mask */
		old_dma_mask = dma_get_mask(map->dev);
		ret = dma_set_mask(map->dev, DMA_BIT_MASK(map->bparam.dma_bits));
		if (ret) {
			pr_err("failed to set dma_mask on device %s\n",
				dev_name(map->dev));
			return -EINVAL;
		}

		/* Choose benchmark type based on use_iova field */
		if (map->bparam.use_iova == 2) {
			/* Both regular and IOVA */
			ret = do_streaming_iova_benchmark(map);
		} else if (map->bparam.use_iova == 1) {
			/* IOVA only */
			ret = do_iova_benchmark(map);
		}

		/* Restore original DMA mask */
		dma_set_mask(map->dev, old_dma_mask);

		if (ret)
			return ret;
		break;

	default:
		return -EINVAL;
	}

	if (copy_to_user(argp, &map->bparam, sizeof(map->bparam)))
		return -EFAULT;

	return ret;
}

static const struct file_operations map_benchmark_fops = {
	.open			= simple_open,
	.unlocked_ioctl		= map_benchmark_ioctl,
};

static void map_benchmark_remove_debugfs(void *data)
{
	struct map_benchmark_data *map = (struct map_benchmark_data *)data;

	debugfs_remove(map->debugfs);
}

static int __map_benchmark_probe(struct device *dev)
{
	struct dentry *entry;
	struct map_benchmark_data *map;
	int ret;

	map = devm_kzalloc(dev, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;
	map->dev = dev;

	ret = devm_add_action(dev, map_benchmark_remove_debugfs, map);
	if (ret) {
		pr_err("Can't add debugfs remove action\n");
		return ret;
	}

	/*
	 * we only permit a device bound with this driver, 2nd probe
	 * will fail
	 */
	entry = debugfs_create_file("dma_map_benchmark", 0600, NULL, map,
			&map_benchmark_fops);
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	map->debugfs = entry;

	return 0;
}

static int map_benchmark_platform_probe(struct platform_device *pdev)
{
	return __map_benchmark_probe(&pdev->dev);
}

static struct platform_driver map_benchmark_platform_driver = {
	.driver		= {
		.name	= "dma_map_benchmark",
	},
	.probe = map_benchmark_platform_probe,
};

static int
map_benchmark_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	return __map_benchmark_probe(&pdev->dev);
}

static struct pci_driver map_benchmark_pci_driver = {
	.name	= "dma_map_benchmark",
	.probe	= map_benchmark_pci_probe,
};

static int __init map_benchmark_init(void)
{
	int ret;

	ret = pci_register_driver(&map_benchmark_pci_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&map_benchmark_platform_driver);
	if (ret) {
		pci_unregister_driver(&map_benchmark_pci_driver);
		return ret;
	}

	return 0;
}

static void __exit map_benchmark_cleanup(void)
{
	platform_driver_unregister(&map_benchmark_platform_driver);
	pci_unregister_driver(&map_benchmark_pci_driver);
}

module_init(map_benchmark_init);
module_exit(map_benchmark_cleanup);

MODULE_AUTHOR("Barry Song <song.bao.hua@hisilicon.com>");
MODULE_DESCRIPTION("dma_map benchmark driver");
