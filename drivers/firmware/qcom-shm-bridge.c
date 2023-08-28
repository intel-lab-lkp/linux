// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Linaro Limited
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/device/bus.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/shm-bridge.h>
#include <linux/genalloc.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/radix-tree.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define QCOM_SHM_BRIDGE_NUM_VM_SHIFT 9

DEFINE_FREE(qcom_shm_bridge_pool, struct qcom_shm_bridge_pool *,
	    if (_T) qcom_shm_bridge_pool_unref(_T))

/*
 * Size of the global fall-back pool can be adjusted using this param. Use 4M
 * as a sane default.
 */
static unsigned int qcom_shm_bridge_default_pool_size = SZ_4M;
module_param_named(default_pool_size, qcom_shm_bridge_default_pool_size,
		   uint, 0644);

struct qcom_shm_bridge_pool {
	struct device *dev;
	void *vaddr;
	phys_addr_t paddr;
	size_t size;
	uint64_t handle;
	struct gen_pool *genpool;
	struct list_head chunks;
	spinlock_t lock;
	struct kref refcount;
};

struct qcom_shm_bridge_chunk {
	void *vaddr;
	size_t size;
	struct qcom_shm_bridge_pool *parent;
	struct list_head siblings;
};

/* This is the global fall-back pool, used if user doesn't supply their own. */
static struct qcom_shm_bridge_pool *qcom_shm_bridge_default_pool;

static RADIX_TREE(qcom_shm_bridge_chunks, GFP_ATOMIC);
static DEFINE_SPINLOCK(qcom_shm_bridge_chunks_lock);

static void qcom_shm_bridge_pool_release(struct kref *kref)
{
	struct qcom_shm_bridge_pool *pool =
		container_of(kref, struct qcom_shm_bridge_pool, refcount);

	if (pool->handle)
		qcom_scm_delete_shm_bridge(pool->dev, pool->handle);

	if (pool->genpool)
		gen_pool_destroy(pool->genpool);

	if (pool->paddr)
		dma_unmap_single(pool->dev, pool->paddr, pool->size,
				 DMA_TO_DEVICE);

	if (pool->vaddr)
		free_pages((unsigned long)pool->vaddr, get_order(pool->size));

	put_device(pool->dev);
	kfree(pool);
}

static int qcom_shm_bridge_create(struct qcom_shm_bridge_pool *pool)
{
	uint64_t pfn_and_ns_perm, ipfn_and_s_perm, size_and_flags, ns_vmids,
		 ns_perms, handle;
	int ret;

	ns_perms = (QCOM_SCM_PERM_WRITE | QCOM_SCM_PERM_READ);
	ns_vmids = QCOM_SCM_VMID_HLOS;
	pfn_and_ns_perm = (u64)pool->paddr | ns_perms;
	ipfn_and_s_perm = (u64)pool->paddr | ns_perms;
	size_and_flags = pool->size | (1 << QCOM_SHM_BRIDGE_NUM_VM_SHIFT);

	ret = qcom_scm_create_shm_bridge(pool->dev, pfn_and_ns_perm,
					 ipfn_and_s_perm, size_and_flags,
					 ns_vmids, &handle);
	if (!ret)
		pool->handle = handle;

	return ret;
}

static struct qcom_shm_bridge_pool *
qcom_shm_bridge_pool_new_for_dev(struct device *dev, size_t size)
{
	struct qcom_shm_bridge_pool *pool __free(qcom_shm_bridge_pool) = NULL;
	int ret;

	if (!qcom_scm_shm_bridge_available())
		return ERR_PTR(-ENODEV);

	if (!size)
		return ERR_PTR(-EINVAL);

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	pool->dev = get_device(dev);
	pool->size = size;
	INIT_LIST_HEAD(&pool->chunks);
	spin_lock_init(&pool->lock);
	kref_init(&pool->refcount);

	pool->vaddr = (void *)__get_free_pages(GFP_KERNEL | GFP_DMA,
					       get_order(pool->size));
	if (!pool->vaddr)
		return ERR_PTR(-ENOMEM);

	pool->paddr = dma_map_single(pool->dev, pool->vaddr, pool->size,
				     DMA_TO_DEVICE);
	if (dma_mapping_error(dev, pool->paddr))
		return ERR_PTR(-ENOMEM);

	pool->genpool = gen_pool_create(PAGE_SHIFT, -1);
	if (!pool->genpool)
		return ERR_PTR(-ENOMEM);

	gen_pool_set_algo(pool->genpool, gen_pool_best_fit, NULL);

	ret = gen_pool_add_virt(pool->genpool, (unsigned long)pool->vaddr,
				pool->paddr, pool->size, -1);
	if (ret)
		return ERR_PTR(ret);

	ret = qcom_shm_bridge_create(pool);
	if (ret)
		return ERR_PTR(ret);

	return no_free_ptr(pool);
}

/**
 * qcom_shm_bridge_pool_new - Create a new SHM Bridge memory pool.
 *
 * @size: Size of the pool.
 *
 * Creates a new Shared Memory Bridge pool from which users can allocate memory
 * chunks. Must be called from process context.
 *
 * Return:
 * Pointer to the newly created SHM Bridge pool with reference count set to 1
 * or ERR_PTR().
 */
struct qcom_shm_bridge_pool *qcom_shm_bridge_pool_new(size_t size)
{
	struct device *dev;

	dev = bus_find_device_by_name(&platform_bus_type, NULL,
				      "qcom-shm-bridge");
	if (!dev)
		return ERR_PTR(-ENODEV);

	return qcom_shm_bridge_pool_new_for_dev(dev, size);
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_pool_new);

/**
 * qcom_shm_bridge_pool_ref - Increate the refcount of an SHM Bridge pool.
 *
 * @pool: SHM Bridge pool of which the reference count to increase.
 *
 * Return:
 * Pointer to the same pool object.
 */
struct qcom_shm_bridge_pool *
qcom_shm_bridge_pool_ref(struct qcom_shm_bridge_pool *pool)
{
	kref_get(&pool->refcount);

	return pool;
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_pool_ref);

/**
 * qcom_shm_bridge_pool_unref - Decrease the refcount of an SHM Bridge pool.
 *
 * @pool: SHM Bridge pool of which the reference count to decrease.
 *
 * Once the reference count reaches 0, the pool is released.
 */
void qcom_shm_bridge_pool_unref(struct qcom_shm_bridge_pool *pool)
{
	kref_put(&pool->refcount, qcom_shm_bridge_pool_release);
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_pool_unref);

static void devm_qcom_shm_bridge_pool_unref(void *data)
{
	struct qcom_shm_bridge_pool *pool = data;

	qcom_shm_bridge_pool_unref(pool);
}

/**
 * devm_qcom_shm_bridge_pool_new - Managed variant of qcom_shm_bridge_pool_new.
 *
 * @dev: Device for which to map memory and which will manage this pool.
 * @size: Size of the pool.
 *
 * Return:
 * Pointer to the newly created SHM Bridge pool with reference count set to 1
 * or ERR_PTR().
 */
struct qcom_shm_bridge_pool *
devm_qcom_shm_bridge_pool_new(struct device *dev, size_t size)
{
	struct qcom_shm_bridge_pool *pool;
	int ret;

	pool = qcom_shm_bridge_pool_new(size);
	if (IS_ERR(pool))
		return pool;

	ret = devm_add_action_or_reset(dev, devm_qcom_shm_bridge_pool_unref,
				       pool);
	if (ret)
		return ERR_PTR(ret);

	return pool;
}
EXPORT_SYMBOL_GPL(devm_qcom_shm_bridge_pool_new);

/**
 * qcom_shm_bridge_alloc - Allocate a chunk of memory from an SHM Bridge pool.
 *
 * @pool: Pool to allocate memory from. May be NULL.
 * @size: Number of bytes to allocate.
 * @gfp: Allocation flags.
 *
 * If pool is NULL then the global fall-back pool is used.
 *
 * Return:
 * Virtual address of the allocated memory or ERR_PTR(). Must be freed using
 * qcom_shm_bridge_free().
 */
void *qcom_shm_bridge_alloc(struct qcom_shm_bridge_pool *pool,
			    size_t size, gfp_t gfp)
{
	struct qcom_shm_bridge_chunk *chunk __free(kfree) = NULL;
	unsigned long vaddr;
	int ret;

	if (!pool) {
		pool = READ_ONCE(qcom_shm_bridge_default_pool);
		if (!pool)
			return ERR_PTR(-ENODEV);
	}

	if (!size || size > pool->size)
		return ERR_PTR(-EINVAL);

	size = roundup(size, 1 << PAGE_SHIFT);

	chunk = kzalloc(sizeof(*chunk), gfp);
	if (!chunk)
		return ERR_PTR(-ENOMEM);

	guard(spinlock_irqsave)(&pool->lock);

	vaddr = gen_pool_alloc(pool->genpool, size);
	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	chunk->vaddr = (void *)vaddr;
	chunk->size = size;
	chunk->parent = pool;
	list_add_tail(&chunk->siblings, &pool->chunks);
	qcom_shm_bridge_pool_ref(pool);

	guard(spinlock_irqsave)(&qcom_shm_bridge_chunks_lock);

	ret = radix_tree_insert(&qcom_shm_bridge_chunks, vaddr, chunk);
	if (ret) {
		gen_pool_free(pool->genpool, vaddr, chunk->size);
		return ERR_PTR(ret);
	}

	return no_free_ptr(chunk)->vaddr;
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_alloc);

/**
 * qcom_shm_bridge_free - Free SHM Bridge memory allocated from the pool.
 *
 * @vaddr: Virtual address of the allocated memory to free.
 */
void qcom_shm_bridge_free(void *vaddr)
{
	struct qcom_shm_bridge_chunk *chunk;
	struct qcom_shm_bridge_pool *pool;

	scoped_guard(spinlock_irqsave, &qcom_shm_bridge_chunks_lock)
		chunk = radix_tree_delete_item(&qcom_shm_bridge_chunks,
					       (unsigned long)vaddr, NULL);
	if (!chunk)
		goto out_warn;

	pool = chunk->parent;

	guard(spinlock_irqsave)(&pool->lock);

	list_for_each_entry(chunk, &pool->chunks, siblings) {
		if (vaddr != chunk->vaddr)
			continue;

		gen_pool_free(pool->genpool, (unsigned long)chunk->vaddr,
			      chunk->size);
		list_del(&chunk->siblings);
		qcom_shm_bridge_pool_unref(pool);
		kfree(chunk);
		return;
	}

out_warn:
	WARN(1, "Virtual address %p not allocated for SHM bridge", vaddr);
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_free);

/**
 * devm_qcom_shm_bridge_alloc - Managed variant of qcom_shm_bridge_alloc.
 *
 * @dev: Managing device.
 * @pool: Pool to allocate memory from.
 * @size: Number of bytes to allocate.
 * @gfp: Allocation flags.
 *
 * Return:
 * Virtual address of the allocated memory or ERR_PTR().
 */
void *devm_qcom_shm_bridge_alloc(struct device *dev,
				 struct qcom_shm_bridge_pool *pool,
				 size_t size, gfp_t gfp)
{
	void *vaddr;
	int ret;

	vaddr = qcom_shm_bridge_alloc(pool, size, gfp);
	if (IS_ERR(vaddr))
		return vaddr;

	ret = devm_add_action_or_reset(dev, qcom_shm_bridge_free, vaddr);
	if (ret)
		return ERR_PTR(ret);

	return vaddr;
}
EXPORT_SYMBOL_GPL(devm_qcom_shm_bridge_alloc);

/**
 * qcom_shm_bridge_to_phys_addr - Translate address from virtual to physical.
 *
 * @vaddr: Virtual address to translate.
 *
 * Return:
 * Physical address corresponding to 'vaddr'.
 */
phys_addr_t qcom_shm_bridge_to_phys_addr(void *vaddr)
{
	struct qcom_shm_bridge_chunk *chunk;
	struct qcom_shm_bridge_pool *pool;

	guard(spinlock_irqsave)(&qcom_shm_bridge_chunks_lock);

	chunk = radix_tree_lookup(&qcom_shm_bridge_chunks,
				  (unsigned long)vaddr);
	if (!chunk)
		return 0;

	pool = chunk->parent;

	guard(spinlock_irqsave)(&pool->lock);

	return gen_pool_virt_to_phys(pool->genpool, (unsigned long)vaddr);
}
EXPORT_SYMBOL_GPL(qcom_shm_bridge_to_phys_addr);

static int qcom_shm_bridge_probe(struct platform_device *pdev)
{
	struct qcom_shm_bridge_pool *default_pool;
	struct device *dev = &pdev->dev;
	int ret;

	/*
	 * We need to wait for the SCM device to be created and bound to the
	 * SCM driver.
	 */
	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	ret = qcom_scm_enable_shm_bridge();
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to enable the SHM bridge\n");

	default_pool = qcom_shm_bridge_pool_new_for_dev(
				dev, qcom_shm_bridge_default_pool_size);
	if (IS_ERR(default_pool))
		return dev_err_probe(dev, PTR_ERR(default_pool),
				     "Failed to create the default SHM Bridge pool\n");

	WRITE_ONCE(qcom_shm_bridge_default_pool, default_pool);

	return 0;
}

static const struct of_device_id qcom_shm_bridge_of_match[] = {
	{ .compatible = "qcom,shm-bridge", },
	{ }
};

static struct platform_driver qcom_shm_bridge_driver = {
	.driver = {
		.name = "qcom-shm-bridge",
		.of_match_table = qcom_shm_bridge_of_match,
		/*
		 * Once enabled, the SHM Bridge feature cannot be disabled so
		 * there's no reason to ever unbind the driver.
		 */
		.suppress_bind_attrs = true,
	},
	.probe = qcom_shm_bridge_probe,
};

static int __init qcom_shm_bridge_init(void)
{
	return platform_driver_register(&qcom_shm_bridge_driver);
}
subsys_initcall(qcom_shm_bridge_init);
