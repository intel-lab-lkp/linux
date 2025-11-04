// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2021 Intel Corporation. All rights rsvd. */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <uapi/linux/idxd.h>
#include <linux/highmem.h>
#include <linux/sched/smt.h>
#include <crypto/internal/acompress.h>

#include "idxd.h"
#include "iaa_crypto.h"
#include "iaa_crypto_stats.h"

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt)			"idxd: " IDXD_SUBDRIVER_NAME ": " fmt

#define IAA_ALG_PRIORITY               300
#define MAX_PKG_IAA   8
#define MAX_IAA_WQ    8

/**************************************
 * Driver internal global variables.
 **************************************/

/* number of iaa instances probed */
static atomic_t nr_iaa = ATOMIC_INIT(0);
static unsigned int nr_cpus;
static unsigned int nr_packages;
static unsigned int nr_cpus_per_package;
static atomic_t nr_iaa_per_package = ATOMIC_INIT(0);

/* Number of physical cpus sharing each iaa instance */
static atomic_t cpus_per_iaa = ATOMIC_INIT(0);

/* Per-cpu lookup table for decomp wqs. */
static struct wq_table_entry __percpu *cpu_decomp_wqs;

/* Per-cpu lookup table for comp wqs. */
static struct wq_table_entry __percpu *cpu_comp_wqs;

/* All decomp wqs from IAAs on a package. */
static struct wq_table_entry **pkg_global_decomp_wqs;
/* All comp wqs from IAAs on a package. */
static struct wq_table_entry **pkg_global_comp_wqs;

/* For software deflate fallback compress/decompress. */
static struct crypto_acomp *deflate_crypto_acomp;
DEFINE_MUTEX(deflate_crypto_acomp_lock);

/* Per-cpu iaa_reqs for batching. */
static struct iaa_batch_ctx __percpu *iaa_batch_ctx;

LIST_HEAD(iaa_devices);
DEFINE_MUTEX(iaa_devices_lock);

/*
 * If enabled, IAA hw crypto algos are registered, unavailable otherwise:
 *
 * We use the atomic @iaa_crypto_enabled to know if the per-CPU
 * compress/decompress wq tables have been setup successfully.
 * Since @iaa_crypto_enabled is atomic, the core functions that
 * return a wq for compression/decompression, namely,
 * comp_wq_table_next_wq() and decomp_wq_table_next_wq() will
 * test this atomic before proceeding to query the per-cpu wq tables.
 *
 * These events will set @iaa_crypto_enabled to 1:
 * - Successful rebalance_wq_table() after individual wq addition/removal.
 *
 * These events will set @iaa_crypto_enabled to 0:
 * - Error during rebalance_wq_table() after individual wq addition/removal.
 * - check_completion() timeouts.
 * - @nr_iaa is 0.
 * - module cleanup.
 */
static atomic_t iaa_crypto_enabled = ATOMIC_INIT(0);

/*
 * First wq probed, to use until @iaa_crypto_enabled is 1:
 *
 * The first wq probed will be entered in the per-CPU comp/decomp wq tables
 * until the IAA compression modes are registered. This is done to facilitate
 * the compress/decompress calls from the crypto testmgr resulting from
 * calling crypto_register_acomp().
 *
 * With the new dynamic package-level rebalancing of WQs being
 * discovered asynchronously and concurrently with tests
 * triggered from device registration, this is needed to
 * determine when it is safe for the rebalancing of decomp/comp
 * WQs to de-allocate the per-package WQs and re-allocate them
 * based on the latest number of IAA devices and WQs.
 */
static struct idxd_wq *first_wq_found;
DEFINE_MUTEX(first_wq_found_lock);

const char *iaa_compression_mode_names[IAA_COMP_MODES_MAX] = {
	"fixed",
};

const char *iaa_compression_alg_names[IAA_COMP_MODES_MAX] = {
	"deflate-iaa",
};

static struct iaa_compression_mode *iaa_compression_modes[IAA_COMP_MODES_MAX];
static struct iaa_compression_ctx *iaa_ctx[IAA_COMP_MODES_MAX];
static bool iaa_mode_registered[IAA_COMP_MODES_MAX];
static u8 num_iaa_modes_registered;

/* Distribute decompressions across all IAAs on the package. */
static bool iaa_distribute_decomps;

/* Distribute compressions across all IAAs on the package. */
static bool iaa_distribute_comps = true;

/* Verify results of IAA compress or not */
static bool iaa_verify_compress;

/*
 * The iaa crypto driver supports three 'sync' methods determining how
 * compressions and decompressions are performed:
 *
 * - sync:      the compression or decompression completes before
 *              returning.  This is the mode used by the async crypto
 *              interface when the sync mode is set to 'sync' and by
 *              the sync crypto interface regardless of setting.
 *
 * - async:     the compression or decompression is submitted and returns
 *              immediately.  Completion interrupts are not used so
 *              the caller is responsible for polling the descriptor
 *              for completion.  This mode is applicable to only the
 *              async crypto interface and is ignored for anything
 *              else.
 *
 * - async_irq: the compression or decompression is submitted and
 *              returns immediately.  Completion interrupts are
 *              enabled so the caller can wait for the completion and
 *              yield to other threads.  When the compression or
 *              decompression completes, the completion is signaled
 *              and the caller awakened.  This mode is applicable to
 *              only the async crypto interface and is ignored for
 *              anything else.
 *
 * These modes can be set using the iaa_crypto sync_mode driver
 * attribute.
 */

/* Use async mode */
static bool async_mode = true;
/* Use interrupts */
static bool use_irq;

/* Number of compress-only wqs per iaa*/
static unsigned int g_comp_wqs_per_iaa = 1;

/**************************************************
 * Driver attributes along with get/set functions.
 **************************************************/

static ssize_t verify_compress_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%d\n", iaa_verify_compress);
}

static ssize_t verify_compress_store(struct device_driver *driver,
				     const char *buf, size_t count)
{
	int ret = -EBUSY;

	mutex_lock(&iaa_devices_lock);

	if (atomic_read(&iaa_crypto_enabled))
		goto out;

	ret = kstrtobool(buf, &iaa_verify_compress);
	if (ret)
		goto out;

	ret = count;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
}
static DRIVER_ATTR_RW(verify_compress);

/**
 * set_iaa_sync_mode - Set IAA sync mode
 * @name: The name of the sync mode
 *
 * Make the IAA sync mode named @name the current sync mode used by
 * compression/decompression.
 */

static int set_iaa_sync_mode(const char *name)
{
	int ret = 0;

	if (sysfs_streq(name, "sync")) {
		async_mode = false;
		use_irq = false;
	} else if (sysfs_streq(name, "async")) {
		async_mode = true;
		use_irq = false;
	} else if (sysfs_streq(name, "async_irq")) {
		async_mode = true;
		use_irq = true;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

static ssize_t sync_mode_show(struct device_driver *driver, char *buf)
{
	int ret = 0;

	if (!async_mode && !use_irq)
		ret = sprintf(buf, "%s\n", "sync");
	else if (async_mode && !use_irq)
		ret = sprintf(buf, "%s\n", "async");
	else if (async_mode && use_irq)
		ret = sprintf(buf, "%s\n", "async_irq");

	return ret;
}

static ssize_t sync_mode_store(struct device_driver *driver,
			       const char *buf, size_t count)
{
	int ret = -EBUSY;

	mutex_lock(&iaa_devices_lock);

	if (atomic_read(&iaa_crypto_enabled))
		goto out;

	ret = set_iaa_sync_mode(buf);
	if (ret == 0)
		ret = count;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
}
static DRIVER_ATTR_RW(sync_mode);

static ssize_t g_comp_wqs_per_iaa_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%u\n", g_comp_wqs_per_iaa);
}

static ssize_t g_comp_wqs_per_iaa_store(struct device_driver *driver,
				   const char *buf, size_t count)
{
	int ret = -EBUSY;

	mutex_lock(&iaa_devices_lock);

	if (atomic_read(&iaa_crypto_enabled))
		goto out;

	ret = kstrtouint(buf, 10, &g_comp_wqs_per_iaa);
	if (ret)
		goto out;

	ret = count;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
}
static DRIVER_ATTR_RW(g_comp_wqs_per_iaa);

static ssize_t distribute_decomps_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%d\n", iaa_distribute_decomps);
}

static ssize_t distribute_decomps_store(struct device_driver *driver,
					const char *buf, size_t count)
{
	int ret = -EBUSY;

	mutex_lock(&iaa_devices_lock);

	if (atomic_read(&iaa_crypto_enabled))
		goto out;

	ret = kstrtobool(buf, &iaa_distribute_decomps);
	if (ret)
		goto out;

	ret = count;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
}
static DRIVER_ATTR_RW(distribute_decomps);

static ssize_t distribute_comps_show(struct device_driver *driver, char *buf)
{
	return sprintf(buf, "%d\n", iaa_distribute_comps);
}

static ssize_t distribute_comps_store(struct device_driver *driver,
				      const char *buf, size_t count)
{
	int ret = -EBUSY;

	mutex_lock(&iaa_devices_lock);

	if (atomic_read(&iaa_crypto_enabled))
		goto out;

	ret = kstrtobool(buf, &iaa_distribute_comps);
	if (ret)
		goto out;

	ret = count;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
}
static DRIVER_ATTR_RW(distribute_comps);

/****************************
 * Driver compression modes.
 ****************************/

static int find_empty_iaa_compression_mode(void)
{
	int i = -EINVAL;

	for (i = 0; i < IAA_COMP_MODES_MAX; i++) {
		if (iaa_compression_modes[i])
			continue;
		break;
	}

	return i;
}

static struct iaa_compression_mode *find_iaa_compression_mode(const char *name, int *idx)
{
	struct iaa_compression_mode *mode;
	int i;

	for (i = 0; i < IAA_COMP_MODES_MAX; i++) {
		mode = iaa_compression_modes[i];
		if (!mode)
			continue;

		if (!strcmp(mode->name, name)) {
			*idx = i;
			return iaa_compression_modes[i];
		}
	}

	return NULL;
}

static bool iaa_alg_is_registered(const char *name, int *idx)
{
	int i;

	for (i = 0; i < IAA_COMP_MODES_MAX; ++i) {
		if (!strcmp(name, iaa_compression_alg_names[i]) && iaa_mode_registered[i]) {
			*idx = i;
			return true;
		}
	}

	return false;
}

static void free_iaa_compression_mode(struct iaa_compression_mode *mode)
{
	kfree(mode->name);
	kfree(mode->ll_table);
	kfree(mode->d_table);

	kfree(mode);
}

/*
 * IAA Compression modes are defined by an ll_table and a d_table.
 * These tables are typically generated and captured using statistics
 * collected from running actual compress/decompress workloads.
 *
 * When a new compression mode is added, the tables are saved in a
 * global compression mode list.  When IAA devices are added, a
 * per-IAA device dma mapping is created for each IAA device, for each
 * compression mode.  These are the tables used to do the actual
 * compression/deccompression and are unmapped if/when the devices are
 * removed.  Currently, compression modes must be added before any
 * device is added, and removed after all devices have been removed.
 */

/**
 * remove_iaa_compression_mode - Remove an IAA compression mode
 * @name: The name the compression mode will be known as
 *
 * Remove the IAA compression mode named @name.
 */
void remove_iaa_compression_mode(const char *name)
{
	struct iaa_compression_mode *mode;
	int idx;

	mutex_lock(&iaa_devices_lock);

	if (!list_empty(&iaa_devices))
		goto out;

	mode = find_iaa_compression_mode(name, &idx);
	if (mode) {
		free_iaa_compression_mode(mode);
		iaa_compression_modes[idx] = NULL;
	}
out:
	mutex_unlock(&iaa_devices_lock);
}

/**
 * add_iaa_compression_mode - Add an IAA compression mode
 * @name: The name the compression mode will be known as
 * @ll_table: The ll table
 * @ll_table_size: The ll table size in bytes
 * @d_table: The d table
 * @d_table_size: The d table size in bytes
 * @init: Optional callback function to init the compression mode data
 * @free: Optional callback function to free the compression mode data
 *
 * Add a new IAA compression mode named @name.
 *
 * Returns 0 if successful, errcode otherwise.
 */
int add_iaa_compression_mode(const char *name,
			     const u32 *ll_table,
			     int ll_table_size,
			     const u32 *d_table,
			     int d_table_size,
			     iaa_dev_comp_init_fn_t init,
			     iaa_dev_comp_free_fn_t free)
{
	struct iaa_compression_mode *mode;
	int idx, ret = -ENOMEM;

	mutex_lock(&iaa_devices_lock);

	if (!list_empty(&iaa_devices)) {
		ret = -EBUSY;
		goto out;
	}

	mode = kzalloc(sizeof(*mode), GFP_KERNEL);
	if (!mode)
		goto out;

	mode->name = kstrdup(name, GFP_KERNEL);
	if (!mode->name)
		goto free;

	if (ll_table) {
		mode->ll_table = kmemdup(ll_table, ll_table_size, GFP_KERNEL);
		if (!mode->ll_table)
			goto free;
		mode->ll_table_size = ll_table_size;
	}

	if (d_table) {
		mode->d_table = kmemdup(d_table, d_table_size, GFP_KERNEL);
		if (!mode->d_table)
			goto free;
		mode->d_table_size = d_table_size;
	}

	mode->init = init;
	mode->free = free;

	idx = find_empty_iaa_compression_mode();
	if (idx < 0)
		goto free;

	pr_debug("IAA compression mode %s added at idx %d\n",
		 mode->name, idx);

	iaa_compression_modes[idx] = mode;
	++num_iaa_modes_registered;

	ret = 0;
out:
	mutex_unlock(&iaa_devices_lock);

	return ret;
free:
	free_iaa_compression_mode(mode);
	goto out;
}

static void free_device_compression_mode(struct iaa_device *iaa_device,
					 struct iaa_device_compression_mode *device_mode)
{
	size_t size = sizeof(struct aecs_comp_table_record) + IAA_AECS_ALIGN;
	struct device *dev = &iaa_device->idxd->pdev->dev;

	kfree(device_mode->name);

	if (device_mode->aecs_comp_table)
		dma_free_coherent(dev, size, device_mode->aecs_comp_table,
				  device_mode->aecs_comp_table_dma_addr);
	kfree(device_mode);
}

#define IDXD_OP_FLAG_AECS_RW_TGLS       0x400000
#define IAX_AECS_DEFAULT_FLAG (IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC)
#define IAX_AECS_COMPRESS_FLAG	(IAX_AECS_DEFAULT_FLAG | IDXD_OP_FLAG_RD_SRC2_AECS)
#define IAX_AECS_DECOMPRESS_FLAG (IAX_AECS_DEFAULT_FLAG | IDXD_OP_FLAG_RD_SRC2_AECS)
#define IAX_AECS_GEN_FLAG (IAX_AECS_DEFAULT_FLAG | \
						IDXD_OP_FLAG_WR_SRC2_AECS_COMP | \
						IDXD_OP_FLAG_AECS_RW_TGLS)

static int init_device_compression_mode(struct iaa_device *iaa_device,
					struct iaa_compression_mode *mode,
					int idx, struct idxd_wq *wq)
{
	size_t size = sizeof(struct aecs_comp_table_record) + IAA_AECS_ALIGN;
	struct device *dev = &iaa_device->idxd->pdev->dev;
	struct iaa_device_compression_mode *device_mode;
	int ret = -ENOMEM;

	device_mode = kzalloc(sizeof(*device_mode), GFP_KERNEL);
	if (!device_mode)
		return -ENOMEM;

	device_mode->name = kstrdup(mode->name, GFP_KERNEL);
	if (!device_mode->name)
		goto free;

	device_mode->aecs_comp_table = dma_alloc_coherent(dev, size,
							  &device_mode->aecs_comp_table_dma_addr, GFP_KERNEL);
	if (!device_mode->aecs_comp_table)
		goto free;

	/* Add Huffman table to aecs */
	memset(device_mode->aecs_comp_table, 0, sizeof(*device_mode->aecs_comp_table));
	memcpy(device_mode->aecs_comp_table->ll_sym, mode->ll_table, mode->ll_table_size);
	memcpy(device_mode->aecs_comp_table->d_sym, mode->d_table, mode->d_table_size);

	if (mode->init) {
		ret = mode->init(device_mode);
		if (ret)
			goto free;
	}

	/* mode index should match iaa_compression_modes idx */
	iaa_device->compression_modes[idx] = device_mode;

	pr_debug("IAA %s compression mode initialized for iaa device %d\n",
		 mode->name, iaa_device->idxd->id);

	ret = 0;
out:
	return ret;
free:
	pr_debug("IAA %s compression mode initialization failed for iaa device %d\n",
		 mode->name, iaa_device->idxd->id);

	free_device_compression_mode(iaa_device, device_mode);
	goto out;
}

static int init_device_compression_modes(struct iaa_device *iaa_device,
					 struct idxd_wq *wq)
{
	struct iaa_compression_mode *mode;
	int i, ret = 0;

	for (i = 0; i < IAA_COMP_MODES_MAX; i++) {
		mode = iaa_compression_modes[i];
		if (!mode)
			continue;

		ret = init_device_compression_mode(iaa_device, mode, i, wq);
		if (ret)
			break;
	}

	return ret;
}

static void remove_device_compression_modes(struct iaa_device *iaa_device)
{
	struct iaa_device_compression_mode *device_mode;
	int i;

	for (i = 0; i < IAA_COMP_MODES_MAX; i++) {
		device_mode = iaa_device->compression_modes[i];
		if (!device_mode)
			continue;

		if (iaa_compression_modes[i]->free)
			iaa_compression_modes[i]->free(device_mode);
		free_device_compression_mode(iaa_device, device_mode);
		iaa_device->compression_modes[i] = NULL;
	}
}

/***********************************************************
 * Functions for use in crypto probe and remove interfaces:
 * allocate/init/query/deallocate devices/wqs.
 ***********************************************************/

static struct iaa_device *iaa_device_alloc(struct idxd_device *idxd)
{
	struct iaa_device *iaa_device;
	struct wq_table_entry *wqt;

	iaa_device = kzalloc(sizeof(*iaa_device), GFP_KERNEL);
	if (!iaa_device)
		goto err;

	iaa_device->idxd = idxd;

	/* IAA device's generic/decomp wqs. */
	iaa_device->generic_wq_table = kzalloc(sizeof(struct wq_table_entry), GFP_KERNEL);
	if (!iaa_device->generic_wq_table)
		goto err;

	wqt = iaa_device->generic_wq_table;

	wqt->wqs = kcalloc(iaa_device->idxd->max_wqs, sizeof(struct idxd_wq *), GFP_KERNEL);
	if (!wqt->wqs)
		goto err;

	wqt->max_wqs = iaa_device->idxd->max_wqs;
	wqt->n_wqs = 0;

	/*
	 * IAA device's comp wqs (optional). If the device has more than
	 * "g_comp_wqs_per_iaa" WQs configured, the last "g_comp_wqs_per_iaa"
	 * number of WQs will be considered as "comp only". The remaining
	 * WQs will be considered as "decomp only".
	 * If the device has <= "g_comp_wqs_per_iaa" WQs, all the
	 * device's WQs will be considered "generic", i.e., cores can submit
	 * comp and decomp jobs to all the WQs configured for the device.
	 */
	iaa_device->comp_wq_table = kzalloc(sizeof(struct wq_table_entry), GFP_KERNEL);
	if (!iaa_device->comp_wq_table)
		goto err;

	wqt = iaa_device->comp_wq_table;

	wqt->wqs = kcalloc(iaa_device->idxd->max_wqs, sizeof(struct idxd_wq *), GFP_KERNEL);
	if (!wqt->wqs)
		goto err;

	wqt->max_wqs = iaa_device->idxd->max_wqs;
	wqt->n_wqs = 0;

	INIT_LIST_HEAD(&iaa_device->wqs);

	return iaa_device;

err:
	if (iaa_device) {
		if (iaa_device->generic_wq_table) {
			kfree(iaa_device->generic_wq_table->wqs);
			kfree(iaa_device->generic_wq_table);
		}
		kfree(iaa_device->comp_wq_table);
		kfree(iaa_device);
	}

	return NULL;
}

static struct iaa_device *add_iaa_device(struct idxd_device *idxd)
{
	struct iaa_device *iaa_device;

	iaa_device = iaa_device_alloc(idxd);
	if (!iaa_device)
		return NULL;

	list_add_tail(&iaa_device->list, &iaa_devices);

	atomic_inc(&nr_iaa);

	return iaa_device;
}

static int init_iaa_device(struct iaa_device *iaa_device, struct iaa_wq *iaa_wq)
{
	int ret = 0;

	ret = init_device_compression_modes(iaa_device, iaa_wq->wq);
	if (ret)
		return ret;

	return ret;
}

static void del_iaa_device(struct iaa_device *iaa_device)
{
	list_del(&iaa_device->list);

	atomic_dec(&nr_iaa);
}

static void free_iaa_device(struct iaa_device *iaa_device)
{
	if (!iaa_device || iaa_device->n_wq)
		return;

	remove_device_compression_modes(iaa_device);

	if (iaa_device->generic_wq_table) {
		kfree(iaa_device->generic_wq_table->wqs);
		kfree(iaa_device->generic_wq_table);
	}

	if (iaa_device->comp_wq_table) {
		kfree(iaa_device->comp_wq_table->wqs);
		kfree(iaa_device->comp_wq_table);
	}

	kfree(iaa_device);
}

static bool iaa_has_wq(struct iaa_device *iaa_device, struct idxd_wq *wq)
{
	struct iaa_wq *iaa_wq;

	list_for_each_entry(iaa_wq, &iaa_device->wqs, list) {
		if (iaa_wq->wq == wq)
			return true;
	}

	return false;
}

static void __iaa_wq_release(struct percpu_ref *ref)
{
	struct iaa_wq *iaa_wq = container_of(ref, typeof(*iaa_wq), ref);

	iaa_wq->free = true;
}

static int add_iaa_wq(struct iaa_device *iaa_device, struct idxd_wq *wq,
		      struct iaa_wq **new_wq)
{
	struct idxd_device *idxd = iaa_device->idxd;
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct iaa_wq *iaa_wq;
	int ret;

	iaa_wq = kzalloc(sizeof(*iaa_wq), GFP_KERNEL);
	if (!iaa_wq)
		return -ENOMEM;

	ret = percpu_ref_init(&iaa_wq->ref, __iaa_wq_release,
			      PERCPU_REF_INIT_ATOMIC, GFP_KERNEL);

	if (ret) {
		kfree(iaa_wq);
		return -ENOMEM;
	}

	iaa_wq->wq = wq;
	iaa_wq->iaa_device = iaa_device;
	idxd_wq_set_private(wq, iaa_wq);

	list_add_tail(&iaa_wq->list, &iaa_device->wqs);

	iaa_device->n_wq++;

	if (new_wq)
		*new_wq = iaa_wq;

	dev_dbg(dev, "added wq %d to iaa device %d, n_wq %d\n",
		wq->id, iaa_device->idxd->id, iaa_device->n_wq);

	return 0;
}

static void del_iaa_wq(struct iaa_device *iaa_device, struct idxd_wq *wq)
{
	struct idxd_device *idxd = iaa_device->idxd;
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct iaa_wq *iaa_wq, *next_iaa_wq;

	list_for_each_entry_safe(iaa_wq, next_iaa_wq, &iaa_device->wqs, list) {
		if (iaa_wq->wq == wq) {
			list_del(&iaa_wq->list);
			iaa_device->n_wq--;

			dev_dbg(dev, "removed wq %d from iaa_device %d, n_wq %d, nr_iaa %d\n",
				wq->id, iaa_device->idxd->id,
				iaa_device->n_wq, atomic_read(&nr_iaa));

			if (iaa_device->n_wq == 0)
				del_iaa_device(iaa_device);
			break;
		}
	}
}

static void remove_iaa_wq(struct idxd_wq *wq)
{
	struct iaa_device *iaa_device, *next_iaa_device;
	unsigned int num_pkg_iaa = 0;

	list_for_each_entry_safe(iaa_device, next_iaa_device, &iaa_devices, list) {
		if (iaa_has_wq(iaa_device, wq)) {
			del_iaa_wq(iaa_device, wq);
			break;
		}
	}

	if (atomic_read(&nr_iaa)) {
		atomic_set(&cpus_per_iaa, (nr_packages * nr_cpus_per_package) / atomic_read(&nr_iaa));
		if (!atomic_read(&cpus_per_iaa))
			atomic_set(&cpus_per_iaa, 1);

		num_pkg_iaa = atomic_read(&nr_iaa) / nr_packages;
		if (!num_pkg_iaa)
			num_pkg_iaa = 1;
	} else {
		atomic_set(&cpus_per_iaa, 1);
		num_pkg_iaa = 1;
	}

	atomic_set(&nr_iaa_per_package, num_pkg_iaa);
}

static void __free_iaa_wq(struct iaa_wq *iaa_wq)
{
	struct iaa_device *iaa_device;

	if (!iaa_wq)
		return;

	WARN_ON(!percpu_ref_is_zero(&iaa_wq->ref));
	percpu_ref_exit(&iaa_wq->ref);

	iaa_device = iaa_wq->iaa_device;
	if (iaa_device->n_wq == 0)
		free_iaa_device(iaa_wq->iaa_device);
}

static void free_iaa_wq(struct iaa_wq *iaa_wq)
{
	struct idxd_wq *wq;

	__free_iaa_wq(iaa_wq);

	wq = iaa_wq->wq;

	kfree(iaa_wq);
	idxd_wq_set_private(wq, NULL);
}

static int save_iaa_wq(struct idxd_wq *wq)
{
	struct iaa_device *iaa_device, *found = NULL;
	struct idxd_device *idxd;
	struct pci_dev *pdev;
	struct device *dev;
	int ret = 0;
	unsigned int num_pkg_iaa = 0;

	list_for_each_entry(iaa_device, &iaa_devices, list) {
		if (iaa_device->idxd == wq->idxd) {
			idxd = iaa_device->idxd;
			pdev = idxd->pdev;
			dev = &pdev->dev;
			/*
			 * Check to see that we don't already have this wq.
			 * Shouldn't happen but we don't control probing.
			 */
			if (iaa_has_wq(iaa_device, wq)) {
				dev_dbg(dev, "same wq probed multiple times for iaa_device %p\n",
					iaa_device);
				goto out;
			}

			found = iaa_device;

			ret = add_iaa_wq(iaa_device, wq, NULL);
			if (ret)
				goto out;

			break;
		}
	}

	if (!found) {
		struct iaa_device *new_device;
		struct iaa_wq *new_wq;

		new_device = add_iaa_device(wq->idxd);
		if (!new_device) {
			ret = -ENOMEM;
			goto out;
		}

		ret = add_iaa_wq(new_device, wq, &new_wq);
		if (ret) {
			del_iaa_device(new_device);
			free_iaa_device(new_device);
			goto out;
		}

		ret = init_iaa_device(new_device, new_wq);
		if (ret) {
			del_iaa_wq(new_device, new_wq->wq);
			del_iaa_device(new_device);
			free_iaa_wq(new_wq);
			goto out;
		}
	}

	if (WARN_ON(atomic_read(&nr_iaa) == 0))
		return -EINVAL;

	atomic_set(&cpus_per_iaa, (nr_packages * nr_cpus_per_package) / atomic_read(&nr_iaa));
	if (!atomic_read(&cpus_per_iaa))
		atomic_set(&cpus_per_iaa, 1);

	num_pkg_iaa = atomic_read(&nr_iaa) / nr_packages;
	if (!num_pkg_iaa)
		num_pkg_iaa = 1;

	atomic_set(&nr_iaa_per_package, num_pkg_iaa);

out:
	return 0;
}

/***************************************************************
 * Mapping IAA devices and wqs to cores with per-cpu wq_tables.
 ***************************************************************/

/*
 * Given a cpu, find the closest IAA instance.
 */
static inline int cpu_to_iaa(int cpu)
{
	int package_id, base_iaa, iaa = 0;

	if (!nr_packages || !atomic_read(&nr_iaa_per_package) || !atomic_read(&nr_iaa))
		return -1;

	package_id = topology_logical_package_id(cpu);
	base_iaa = package_id * atomic_read(&nr_iaa_per_package);
	iaa = base_iaa + ((cpu % nr_cpus_per_package) / atomic_read(&cpus_per_iaa));

	pr_debug("cpu = %d, package_id = %d, base_iaa = %d, iaa = %d",
		 cpu, package_id, base_iaa, iaa);

	if (iaa >= 0 && iaa < atomic_read(&nr_iaa))
		return iaa;

	return (atomic_read(&nr_iaa) - 1);
}

static void free_wq_tables(void)
{
	if (cpu_decomp_wqs) {
		free_percpu(cpu_decomp_wqs);
		cpu_decomp_wqs = NULL;
	}

	if (cpu_comp_wqs) {
		free_percpu(cpu_comp_wqs);
		cpu_comp_wqs = NULL;
	}

	pr_debug("freed comp/decomp wq tables\n");
}

static void pkg_global_wqs_dealloc(void)
{
	int i;

	if (pkg_global_decomp_wqs) {
		for (i = 0; i < nr_packages; ++i) {
			kfree(pkg_global_decomp_wqs[i]->wqs);
			kfree(pkg_global_decomp_wqs[i]);
		}
		kfree(pkg_global_decomp_wqs);
		pkg_global_decomp_wqs = NULL;
	}

	if (pkg_global_comp_wqs) {
		for (i = 0; i < nr_packages; ++i) {
			kfree(pkg_global_comp_wqs[i]->wqs);
			kfree(pkg_global_comp_wqs[i]);
		}
		kfree(pkg_global_comp_wqs);
		pkg_global_comp_wqs = NULL;
	}
}

static bool pkg_global_wqs_alloc(void)
{
	int i;

	pkg_global_decomp_wqs = kcalloc(nr_packages, sizeof(*pkg_global_decomp_wqs), GFP_KERNEL);
	if (!pkg_global_decomp_wqs)
		return false;

	for (i = 0; i < nr_packages; ++i) {
		pkg_global_decomp_wqs[i] = kzalloc(sizeof(struct wq_table_entry), GFP_KERNEL);
		if (!pkg_global_decomp_wqs[i])
			goto err;

		pkg_global_decomp_wqs[i]->wqs = kcalloc(MAX_PKG_IAA * MAX_IAA_WQ, sizeof(struct idxd_wq *), GFP_KERNEL);
		if (!pkg_global_decomp_wqs[i]->wqs)
			goto err;

		pkg_global_decomp_wqs[i]->max_wqs = MAX_PKG_IAA * MAX_IAA_WQ;
	}

	pkg_global_comp_wqs = kcalloc(nr_packages, sizeof(*pkg_global_comp_wqs), GFP_KERNEL);
	if (!pkg_global_comp_wqs)
		goto err;

	for (i = 0; i < nr_packages; ++i) {
		pkg_global_comp_wqs[i] = kzalloc(sizeof(struct wq_table_entry), GFP_KERNEL);
		if (!pkg_global_comp_wqs[i])
			goto err;

		pkg_global_comp_wqs[i]->wqs = kcalloc(MAX_PKG_IAA * MAX_IAA_WQ, sizeof(struct idxd_wq *), GFP_KERNEL);
		if (!pkg_global_comp_wqs[i]->wqs)
			goto err;

		pkg_global_comp_wqs[i]->max_wqs = MAX_PKG_IAA * MAX_IAA_WQ;
	}

	return true;

err:
	pkg_global_wqs_dealloc();
	return false;
}

static int alloc_wq_table(int max_wqs)
{
	cpu_decomp_wqs = alloc_percpu_gfp(struct wq_table_entry, GFP_KERNEL | __GFP_ZERO);
	if (!cpu_decomp_wqs)
		return -ENOMEM;

	cpu_comp_wqs = alloc_percpu_gfp(struct wq_table_entry, GFP_KERNEL | __GFP_ZERO);
	if (!cpu_comp_wqs)
		goto err;

	if (!pkg_global_wqs_alloc())
		goto err;

	pr_debug("initialized wq table\n");

	return 0;

err:
	free_wq_tables();
	return -ENOMEM;
}

/*
 * The caller should have established that device_iaa_wqs is not empty,
 * i.e., every IAA device in "iaa_devices" has at least one WQ.
 */
static void add_device_wqs_to_wq_table(struct wq_table_entry *dst_wq_table,
				       struct wq_table_entry *device_wq_table)
{
	int i;

	for (i = 0; i < device_wq_table->n_wqs; ++i)
		dst_wq_table->wqs[dst_wq_table->n_wqs++] = device_wq_table->wqs[i];
}

static bool reinit_pkg_global_wqs(bool comp)
{
	int cur_iaa = 0, pkg = 0;
	struct iaa_device *iaa_device;
	struct wq_table_entry **pkg_wqs = comp ? pkg_global_comp_wqs : pkg_global_decomp_wqs;

	for (pkg = 0; pkg < nr_packages; ++pkg)
		pkg_wqs[pkg]->n_wqs = 0;

	pkg = 0;

one_iaa_special_case:
	/* Re-initialize per-package wqs. */
	list_for_each_entry(iaa_device, &iaa_devices, list) {
		struct wq_table_entry *device_wq_table = comp ?
			((iaa_device->comp_wq_table->n_wqs > 0) ?
				iaa_device->comp_wq_table : iaa_device->generic_wq_table) :
			iaa_device->generic_wq_table;

		if (pkg_wqs[pkg]->n_wqs + device_wq_table->n_wqs > pkg_wqs[pkg]->max_wqs) {
			pkg_wqs[pkg]->wqs = krealloc(pkg_wqs[pkg]->wqs,
						     ksize(pkg_wqs[pkg]->wqs) +
						     max((MAX_PKG_IAA * MAX_IAA_WQ), iaa_device->n_wq) * sizeof(struct idxd_wq *),
						     GFP_KERNEL | __GFP_ZERO);
			if (!pkg_wqs[pkg]->wqs)
				return false;

			pkg_wqs[pkg]->max_wqs = ksize(pkg_wqs[pkg]->wqs)/sizeof(struct idxd_wq *);
		}

		add_device_wqs_to_wq_table(pkg_wqs[pkg], device_wq_table);

		pr_debug("pkg_global_%s_wqs[%d] has %u n_wqs %u max_wqs",
			 (comp ? "comp" : "decomp"), pkg, pkg_wqs[pkg]->n_wqs, pkg_wqs[pkg]->max_wqs);

		if (++cur_iaa == atomic_read(&nr_iaa_per_package)) {
			if (++pkg == nr_packages)
				break;
			cur_iaa = 0;
			if (atomic_read(&nr_iaa) == 1)
				goto one_iaa_special_case;
		}
	}

	return true;
}

static void create_cpu_wq_table(int cpu, struct wq_table_entry *wq_table, bool comp)
{
	struct wq_table_entry *entry = comp ?
		per_cpu_ptr(cpu_comp_wqs, cpu) :
		per_cpu_ptr(cpu_decomp_wqs, cpu);

	if (!atomic_read(&iaa_crypto_enabled)) {
		mutex_lock(&first_wq_found_lock);

		if (WARN_ON(!first_wq_found && !wq_table->n_wqs)) {
			mutex_unlock(&first_wq_found_lock);
			return;
		}

		if (!first_wq_found)
			first_wq_found = wq_table->wqs[0];

		mutex_unlock(&first_wq_found_lock);

		entry->wqs = &first_wq_found;
		entry->max_wqs = 1;
		entry->n_wqs = 1;
		entry->cur_wq = 0;
		pr_debug("%s: cpu %d: added %u first_wq_found for %s wqs up to wq %d.%d\n", __func__,
			 cpu, entry->n_wqs, comp ? "comp":"decomp",
			 entry->wqs[entry->n_wqs - 1]->idxd->id,
			 entry->wqs[entry->n_wqs - 1]->id);
		return;
	}

	entry->wqs = wq_table->wqs;
	entry->max_wqs = wq_table->max_wqs;
	entry->n_wqs = wq_table->n_wqs;
	entry->cur_wq = 0;

	if (entry->n_wqs)
		pr_debug("%s: cpu %d: added %u iaa %s wqs up to wq %d.%d: entry->max_wqs = %u\n", __func__,
			 cpu, entry->n_wqs, comp ? "comp":"decomp",
			 entry->wqs[entry->n_wqs - 1]->idxd->id, entry->wqs[entry->n_wqs - 1]->id,
			 entry->max_wqs);
}

static void set_cpu_wq_table_start_wq(int cpu, bool comp)
{
	struct wq_table_entry *entry = comp ?
		per_cpu_ptr(cpu_comp_wqs, cpu) :
		per_cpu_ptr(cpu_decomp_wqs, cpu);
	unsigned int num_pkg_iaa = atomic_read(&nr_iaa_per_package);

	if (!num_pkg_iaa)
		return;

	int start_wq = (entry->n_wqs / num_pkg_iaa) * (cpu_to_iaa(cpu) % num_pkg_iaa);

	if ((start_wq >= 0) && (start_wq < entry->n_wqs))
		entry->cur_wq = start_wq;
}

static void create_cpu_wq_table_from_pkg_wqs(bool comp)
{
	int cpu;

	/*
	 * All CPU on the same package share the same "package global"
	 * [de]comp_wqs.
	 */
	for (cpu = 0; cpu < nr_cpus; cpu += nr_cpus_per_package) {
		int package_id = topology_logical_package_id(cpu);
		struct wq_table_entry *pkg_wq_table = comp ?
			((pkg_global_comp_wqs[package_id]->n_wqs > 0) ?
				pkg_global_comp_wqs[package_id] : pkg_global_decomp_wqs[package_id])
			: pkg_global_decomp_wqs[package_id];
		int pkg_cpu;

		for (pkg_cpu = cpu; pkg_cpu < cpu + nr_cpus_per_package; ++pkg_cpu) {
			/* Initialize decomp/comp wq_table for CPU. */
			create_cpu_wq_table(pkg_cpu, pkg_wq_table, comp);
			/* Stagger the starting WQ in the package WQ table, for each CPU. */
			set_cpu_wq_table_start_wq(pkg_cpu, comp);
		}
	}
}

static int add_mapped_device_wq_table_for_cpu(int iaa, int cpu, bool comp)
{
	struct iaa_device *iaa_device, *found_device = NULL;
	struct wq_table_entry *device_wq_table;
	int ret = 0, cur_iaa = 0;

	list_for_each_entry(iaa_device, &iaa_devices, list) {
		if (cur_iaa != iaa) {
			cur_iaa++;
			continue;
		}

		found_device = iaa_device;
		dev_dbg(&found_device->idxd->pdev->dev,
			"getting wq from iaa_device %d, cur_iaa %d\n",
			found_device->idxd->id, cur_iaa);
		break;
	}

	if (!found_device) {
		found_device = list_first_entry_or_null(&iaa_devices,
							struct iaa_device, list);
		if (!found_device) {
			pr_debug("couldn't find any iaa devices with wqs!\n");
			ret = -EINVAL;
			goto out;
		}
		cur_iaa = 0;

		dev_dbg(&found_device->idxd->pdev->dev,
			"getting wq from only iaa_device %d, cur_iaa %d\n",
			found_device->idxd->id, cur_iaa);
	}

	device_wq_table = comp ?
		((found_device->comp_wq_table->n_wqs > 0) ?
			found_device->comp_wq_table : found_device->generic_wq_table) :
		found_device->generic_wq_table;

	create_cpu_wq_table(cpu, device_wq_table, comp);

out:
	return ret;
}

static void create_cpu_wq_table_from_mapped_device(bool comp)
{
	int cpu, iaa;

	for_each_possible_cpu(cpu) {
		iaa = cpu_to_iaa(cpu);
		pr_debug("rebalance: cpu=%d iaa=%d\n", cpu, iaa);

		if (WARN_ON(iaa == -1)) {
			pr_debug("rebalance (cpu_to_iaa(%d)) failed!\n", cpu);
			return;
		}

		if (WARN_ON(add_mapped_device_wq_table_for_cpu(iaa, cpu, comp))) {
			pr_debug("could not add any wqs of iaa %d to cpu %d!\n", iaa, cpu);
			return;
		}
	}
}

static int map_iaa_device_wqs(struct iaa_device *iaa_device)
{
	struct wq_table_entry *generic, *for_comps;
	int ret = 0, n_wqs_added = 0;
	struct iaa_wq *iaa_wq;

	generic = iaa_device->generic_wq_table;
	for_comps = iaa_device->comp_wq_table;

	list_for_each_entry(iaa_wq, &iaa_device->wqs, list) {
		if (iaa_wq->mapped && ++n_wqs_added)
			continue;

		pr_debug("iaa_device %p: processing wq %d.%d\n", iaa_device, iaa_device->idxd->id, iaa_wq->wq->id);

		if ((!n_wqs_added || ((n_wqs_added + g_comp_wqs_per_iaa) < iaa_device->n_wq)) &&
			(generic->n_wqs < generic->max_wqs)) {

			generic->wqs[generic->n_wqs++] = iaa_wq->wq;
			pr_debug("iaa_device %p: added decomp wq %d.%d\n", iaa_device, iaa_device->idxd->id, iaa_wq->wq->id);
		} else {
			if (WARN_ON(for_comps->n_wqs == for_comps->max_wqs))
				break;

			for_comps->wqs[for_comps->n_wqs++] = iaa_wq->wq;
			pr_debug("iaa_device %p: added comp wq %d.%d\n", iaa_device, iaa_device->idxd->id, iaa_wq->wq->id);
		}

		iaa_wq->mapped = true;
		++n_wqs_added;
	}

	if (!n_wqs_added && !iaa_device->n_wq) {
		pr_debug("iaa_device %d: couldn't find any iaa wqs!\n", iaa_device->idxd->id);
		ret = -EINVAL;
	}

	return ret;
}

static void map_iaa_devices(void)
{
	struct iaa_device *iaa_device;

	list_for_each_entry(iaa_device, &iaa_devices, list) {
		WARN_ON(map_iaa_device_wqs(iaa_device));
	}
}

/*
 * Rebalance the per-cpu wq table based on available IAA devices/WQs.
 * Three driver parameters control how this algorithm works:
 *
 * - g_comp_wqs_per_iaa:
 *
 *   If multiple WQs are configured for a given device, this setting determines
 *   the number of WQs to be used as "compress only" WQs. The remaining WQs will
 *   be used as "decompress only WQs".
 *   Note that the comp WQ can be the same as the decomp WQ, for e.g., if
 *   g_comp_wqs_per_iaa is 0 (regardless of the # of available WQs per device), or,
 *   if there is only 1 WQ configured for a device (regardless of
 *   g_comp_wqs_per_iaa).
 *
 * - distribute_decomps, distribute_comps:
 *
 *   If this is enabled, all [de]comp WQs found from the IAA devices on a
 *   package, will be aggregated into pkg_global_[de]comp_wqs, then assigned to
 *   each CPU on the package.
 *
 * Note:
 * -----
 * rebalance_wq_table() will return true if it was able to successfully
 * configure comp/decomp wqs for all CPUs, without changing the
 * @iaa_crypto_enabled atomic. The caller can re-enable the use of the wq
 * tables after rebalance_wq_table() returns true, by setting the
 * @iaa_crypto_enabled atomic to 1.
 * In case of any errors, the @iaa_crypto_enabled atomic will be set to 0,
 * and rebalance_wq_table() will return false.
 */
static bool rebalance_wq_table(void)
{
	int cpu;

	if (atomic_read(&nr_iaa) == 0)
		goto err;

	map_iaa_devices();

	pr_info("rebalance: nr_packages=%d, nr_cpus %d, nr_iaa %d, nr_iaa_per_package %d, cpus_per_iaa %d\n",
		nr_packages, nr_cpus, atomic_read(&nr_iaa),
		atomic_read(&nr_iaa_per_package), atomic_read(&cpus_per_iaa));

	if (iaa_distribute_decomps) {
		/* Each CPU uses all IAA devices on package for decomps. */
		if (!reinit_pkg_global_wqs(false))
			goto err;
		create_cpu_wq_table_from_pkg_wqs(false);
	} else {
		/*
		 * Each CPU uses the decomp WQ on the mapped IAA device using
		 * a balanced mapping of cores to IAA.
		 */
		create_cpu_wq_table_from_mapped_device(false);
	}

	if (iaa_distribute_comps) {
		/* Each CPU uses all IAA devices on package for comps. */
		if (!reinit_pkg_global_wqs(true))
			goto err;
		create_cpu_wq_table_from_pkg_wqs(true);
	} else {
		/*
		 * Each CPU uses the comp WQ on the mapped IAA device using
		 * a balanced mapping of cores to IAA.
		 */
		create_cpu_wq_table_from_mapped_device(true);
	}

	/* Verify that each cpu has comp and decomp wqs.*/
	for_each_possible_cpu(cpu) {
		struct wq_table_entry *entry = per_cpu_ptr(cpu_decomp_wqs, cpu);

		if (!entry->wqs || !entry->n_wqs) {
			pr_err("%s: cpu %d does not have decomp_wqs", __func__, cpu);
			goto err;
		}

		entry = per_cpu_ptr(cpu_comp_wqs, cpu);
		if (!entry->wqs || !entry->n_wqs) {
			pr_err("%s: cpu %d does not have comp_wqs", __func__, cpu);
			goto err;
		}
	}

	pr_debug("Finished rebalance decomp/comp wqs.");
	return true;

err:
	atomic_set(&iaa_crypto_enabled, 0);
	pr_debug("Error during rebalance decomp/comp wqs.");
	return false;
}

/***************************************************************
 * Assign work-queues for driver ops using per-cpu wq_tables.
 ***************************************************************/

static struct idxd_wq *decomp_wq_table_next_wq(int cpu)
{
	struct wq_table_entry *entry = per_cpu_ptr(cpu_decomp_wqs, cpu);
	struct idxd_wq *wq;

	if (!atomic_read(&iaa_crypto_enabled))
		return NULL;

	wq = entry->wqs[entry->cur_wq];

	if (++entry->cur_wq == entry->n_wqs)
		entry->cur_wq = 0;

	return wq;
}

static struct idxd_wq *comp_wq_table_next_wq(int cpu)
{
	struct wq_table_entry *entry = per_cpu_ptr(cpu_comp_wqs, cpu);
	struct idxd_wq *wq;

	if (!atomic_read(&iaa_crypto_enabled))
		return NULL;

	wq = entry->wqs[entry->cur_wq];

	if (++entry->cur_wq == entry->n_wqs)
		entry->cur_wq = 0;

	return wq;
}

/*************************************************
 * Core iaa_crypto compress/decompress functions.
 *************************************************/

static int deflate_generic_decompress(struct iaa_req *req)
{
	ACOMP_REQUEST_ON_STACK(fbreq, deflate_crypto_acomp);
	int ret;

	acomp_request_set_callback(fbreq, 0, NULL, NULL);
	acomp_request_set_params(fbreq, req->src, req->dst, req->slen,
				 PAGE_SIZE);

	mutex_lock(&deflate_crypto_acomp_lock);

	ret = crypto_acomp_decompress(fbreq);
	req->dlen = fbreq->dlen;

	mutex_unlock(&deflate_crypto_acomp_lock);

	update_total_sw_decomp_calls();

	return ret;
}

static __always_inline void acomp_to_iaa(struct acomp_req *areq,
					 struct iaa_req *req,
					 struct iaa_compression_ctx *ctx)
{
	req->src = areq->src;
	req->dst = areq->dst;
	req->slen = areq->slen;
	req->dlen = areq->dlen;
	req->flags = areq->base.flags;
	if (unlikely(ctx->use_irq))
		req->drv_data = areq;
}

static __always_inline void iaa_to_acomp(int dlen, struct acomp_req *areq)
{
	areq->dst->length = dlen;
	areq->dlen = dlen;
}

static inline int check_completion(struct device *dev,
				   struct iax_completion_record *comp,
				   bool compress,
				   bool only_once)
{
	char *op_str = compress ? "compress" : "decompress";
	int status_checks = 0;
	int ret = 0;

	while (!comp->status) {
		if (only_once)
			return -EAGAIN;
		cpu_relax();
		if (status_checks++ >= IAA_COMPLETION_TIMEOUT) {
			/* Something is wrong with the hw, disable it. */
			dev_err(dev, "%s completion timed out - "
				"assuming broken hw, iaa_crypto now DISABLED\n",
				op_str);
			atomic_set(&iaa_crypto_enabled, 0);
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	if (comp->status != IAX_COMP_SUCCESS) {
		if (comp->status == IAA_ERROR_WATCHDOG_EXPIRED) {
			ret = -ETIMEDOUT;
			dev_dbg(dev, "%s timed out, size=0x%x\n",
				op_str, comp->output_size);
			update_completion_timeout_errs();
			goto out;
		}

		if (comp->status == IAA_ANALYTICS_ERROR &&
		    comp->error_code == IAA_ERROR_COMP_BUF_OVERFLOW && compress) {
			ret = -E2BIG;
			dev_dbg(dev, "compressed > uncompressed size,"
				" not compressing, size=0x%x\n",
				comp->output_size);
			update_completion_comp_buf_overflow_errs();
			goto out;
		}

		if (comp->status == IAA_ERROR_DECOMP_BUF_OVERFLOW) {
			ret = -EOVERFLOW;
			goto out;
		}

		ret = -EINVAL;
		dev_dbg(dev, "iaa %s status=0x%x, error=0x%x, size=0x%x\n",
			op_str, comp->status, comp->error_code, comp->output_size);
		print_hex_dump(KERN_INFO, "cmp-rec: ", DUMP_PREFIX_OFFSET, 8, 1, comp, 64, 0);
		update_completion_einval_errs();

		goto out;
	}
out:
	return ret;
}

static int iaa_remap_for_verify(struct device *dev, struct iaa_wq *iaa_wq,
				struct iaa_req *req,
				dma_addr_t *src_addr, dma_addr_t *dst_addr)
{
	int ret = 0;
	int nr_sgs;

	dma_unmap_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
	dma_unmap_sg(dev, req->src, 1, DMA_TO_DEVICE);

	nr_sgs = dma_map_sg(dev, req->src, 1, DMA_FROM_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "verify: couldn't map src sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		goto out;
	}
	*src_addr = sg_dma_address(req->src);
	dev_dbg(dev, "verify: dma_map_sg, src_addr %llx, nr_sgs %d, req->src %p,"
		" req->slen %d, sg_dma_len(sg) %d\n", *src_addr, nr_sgs,
		req->src, req->slen, sg_dma_len(req->src));

	nr_sgs = dma_map_sg(dev, req->dst, 1, DMA_TO_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "verify: couldn't map dst sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		dma_unmap_sg(dev, req->src, 1, DMA_FROM_DEVICE);
		goto out;
	}
	*dst_addr = sg_dma_address(req->dst);
	dev_dbg(dev, "verify: dma_map_sg, dst_addr %llx, nr_sgs %d, req->dst %p,"
		" req->dlen %d, sg_dma_len(sg) %d\n", *dst_addr, nr_sgs,
		req->dst, req->dlen, sg_dma_len(req->dst));
out:
	return ret;
}

static int iaa_compress_verify(struct iaa_compression_ctx *ctx, struct iaa_req *req,
			       struct idxd_wq *wq,
			       dma_addr_t src_addr, unsigned int slen,
			       dma_addr_t dst_addr, unsigned int dlen)
{
	struct iaa_device *iaa_device;
	struct idxd_desc *idxd_desc = ERR_PTR(-EAGAIN);
	u16 alloc_desc_retries = 0;
	struct iax_hw_desc *desc;
	struct idxd_device *idxd;
	struct iaa_wq *iaa_wq;
	struct pci_dev *pdev;
	struct device *dev;
	int ret = 0;

	iaa_wq = idxd_wq_get_private(wq);
	iaa_device = iaa_wq->iaa_device;
	idxd = iaa_device->idxd;
	pdev = idxd->pdev;
	dev = &pdev->dev;

	while ((idxd_desc == ERR_PTR(-EAGAIN)) && (alloc_desc_retries++ < ctx->alloc_decomp_desc_timeout)) {
		idxd_desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
		cpu_relax();
	}

	if (IS_ERR(idxd_desc)) {
		dev_dbg(dev, "iaa compress_verify failed: idxd descriptor allocation failure: ret=%ld\n", PTR_ERR(idxd_desc));
		return -ENODEV;
	}
	desc = idxd_desc->iax_hw;

	/* Verify (optional) - decompress and check crc, suppress dest write */

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_DECOMPRESS;
	desc->decompr_flags = IAA_DECOMP_FLAGS | IAA_DECOMP_SUPPRESS_OUTPUT;
	desc->priv = 0;

	desc->src1_addr = (u64)dst_addr;
	desc->src1_size = dlen;
	desc->dst_addr = (u64)src_addr;
	desc->max_dst_size = slen;
	desc->completion_addr = idxd_desc->compl_dma;

	ret = idxd_submit_desc(wq, idxd_desc);
	if (ret) {
		dev_dbg(dev, "submit_desc (verify) failed ret=%d\n", ret);
		goto err;
	}

	ret = check_completion(dev, idxd_desc->iax_completion, false, false);
	if (ret) {
		dev_dbg(dev, "(verify) check_completion failed ret=%d\n", ret);
		goto err;
	}

	if (req->compression_crc != idxd_desc->iax_completion->crc) {
		ret = -EINVAL;
		dev_dbg(dev, "(verify) iaa comp/decomp crc mismatch:"
			" comp=0x%x, decomp=0x%x\n", req->compression_crc,
			idxd_desc->iax_completion->crc);
		print_hex_dump(KERN_INFO, "cmp-rec: ", DUMP_PREFIX_OFFSET,
			       8, 1, idxd_desc->iax_completion, 64, 0);
		goto err;
	}

err:
	idxd_free_desc(wq, idxd_desc);

	return ret;
}

static void iaa_desc_complete(struct idxd_desc *idxd_desc,
			      enum idxd_complete_type comp_type,
			      bool free_desc, void *__ctx,
			      u32 *status)
{
	struct iaa_device_compression_mode *active_compression_mode;
	struct iaa_compression_ctx *compression_ctx;
	struct crypto_ctx *ctx = __ctx;
	struct iaa_device *iaa_device;
	struct idxd_device *idxd;
	struct iaa_wq *iaa_wq;
	struct pci_dev *pdev;
	struct device *dev;
	struct iaa_req req;
	int ret, err = 0;

	compression_ctx = crypto_tfm_ctx(ctx->tfm);

	iaa_wq = idxd_wq_get_private(idxd_desc->wq);
	iaa_device = iaa_wq->iaa_device;
	idxd = iaa_device->idxd;
	pdev = idxd->pdev;
	dev = &pdev->dev;

	active_compression_mode = iaa_device->compression_modes[compression_ctx->mode];
	dev_dbg(dev, "%s: compression mode %s,"
		" ctx->src_addr %llx, ctx->dst_addr %llx\n", __func__,
		active_compression_mode->name,
		ctx->src_addr, ctx->dst_addr);

	ret = check_completion(dev, idxd_desc->iax_completion,
			       ctx->compress, false);
	if (ret) {
		dev_dbg(dev, "%s: check_completion failed ret=%d\n", __func__, ret);
		if (!ctx->compress &&
		    idxd_desc->iax_completion->status == IAA_ANALYTICS_ERROR) {
			pr_warn("%s: falling back to deflate-generic decompress, "
				"analytics error code %x\n", __func__,
				idxd_desc->iax_completion->error_code);

			acomp_to_iaa(ctx->req, &req, compression_ctx);
			ret = deflate_generic_decompress(&req);
			iaa_to_acomp(req.dlen, ctx->req);

			if (ret) {
				dev_dbg(dev, "%s: deflate-generic failed ret=%d\n",
					__func__, ret);
				err = -EIO;
				goto err;
			} else {
				goto verify;
			}
		} else {
			err = -EIO;
			goto err;
		}
	} else {
		ctx->req->dlen = idxd_desc->iax_completion->output_size;
	}

	/* Update stats */
	if (ctx->compress) {
		update_total_comp_bytes_out(ctx->req->dlen);
		update_wq_comp_bytes(iaa_wq->wq, ctx->req->dlen);
	} else {
		update_total_decomp_bytes_in(ctx->req->slen);
		update_wq_decomp_bytes(iaa_wq->wq, ctx->req->slen);
	}

verify:
	if (ctx->compress && compression_ctx->verify_compress) {
		dma_addr_t src_addr, dst_addr;

		acomp_to_iaa(ctx->req, &req, compression_ctx);
		req.compression_crc = idxd_desc->iax_completion->crc;

		ret = iaa_remap_for_verify(dev, iaa_wq, &req, &src_addr, &dst_addr);
		iaa_to_acomp(req.dlen, ctx->req);

		if (ret) {
			dev_dbg(dev, "%s: compress verify remap failed ret=%d\n", __func__, ret);
			err = -EIO;
			goto out;
		}

		ret = iaa_compress_verify(compression_ctx, &req, iaa_wq->wq, src_addr,
					  ctx->req->slen, dst_addr, ctx->req->dlen);
		iaa_to_acomp(req.dlen, ctx->req);

		if (ret) {
			dev_dbg(dev, "%s: compress verify failed ret=%d\n", __func__, ret);
			err = -EIO;
		}

		dma_unmap_sg(dev, ctx->req->dst, 1, DMA_TO_DEVICE);
		dma_unmap_sg(dev, ctx->req->src, 1, DMA_FROM_DEVICE);

		goto out;
	}
err:
	dma_unmap_sg(dev, ctx->req->dst, 1, DMA_FROM_DEVICE);
	dma_unmap_sg(dev, ctx->req->src, 1, DMA_TO_DEVICE);
out:
	if (ret != 0)
		dev_dbg(dev, "asynchronous compress failed ret=%d\n", ret);

	if (ctx->req->base.complete)
		acomp_request_complete(ctx->req, err);

	if (free_desc)
		idxd_free_desc(idxd_desc->wq, idxd_desc);
	percpu_ref_put(&iaa_wq->ref);
}

static struct iax_hw_desc *
iaa_setup_compress_hw_desc(struct idxd_desc *idxd_desc,
			   dma_addr_t src_addr,
			   unsigned int slen,
			   dma_addr_t dst_addr,
			   unsigned int dlen,
			   enum iaa_mode mode,
			   struct iaa_device_compression_mode *active_compression_mode)
{
	struct iax_hw_desc *desc = idxd_desc->iax_hw;

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_COMPRESS;
	desc->compr_flags = IAA_COMP_FLAGS;
	desc->priv = 0;

	desc->src1_addr = (u64)src_addr;
	desc->src1_size = slen;
	desc->dst_addr = (u64)dst_addr;
	desc->max_dst_size = dlen;
	desc->flags |= IDXD_OP_FLAG_RD_SRC2_AECS;
	desc->src2_addr = active_compression_mode->aecs_comp_table_dma_addr;
	desc->src2_size = sizeof(struct aecs_comp_table_record);
	desc->completion_addr = idxd_desc->compl_dma;

	return desc;
}

static struct iax_hw_desc *
iaa_setup_decompress_hw_desc(struct idxd_desc *idxd_desc,
			     dma_addr_t src_addr,
			     unsigned int slen,
			     dma_addr_t dst_addr,
			     unsigned int dlen)
{
	struct iax_hw_desc *desc = idxd_desc->iax_hw;

	desc->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CC;
	desc->opcode = IAX_OPCODE_DECOMPRESS;
	desc->max_dst_size = PAGE_SIZE;
	desc->decompr_flags = IAA_DECOMP_FLAGS;
	desc->priv = 0;

	desc->src1_addr = (u64)src_addr;
	desc->dst_addr = (u64)dst_addr;
	desc->max_dst_size = dlen;
	desc->src1_size = slen;
	desc->completion_addr = idxd_desc->compl_dma;

	return desc;
}

/*
 * Call this for non-irq, non-enqcmds job submissions.
 */
static __always_inline void iaa_submit_desc_movdir64b(struct idxd_wq *wq,
						     struct idxd_desc *desc)
{
	void __iomem *portal = idxd_wq_portal_addr(wq);

	/*
	 * The wmb() flushes writes to coherent DMA data before
	 * possibly triggering a DMA read. The wmb() is necessary
	 * even on UP because the recipient is a device.
	 */
	wmb();

	iosubmit_cmds512(portal, desc->hw, 1);
}

static int iaa_compress(struct iaa_compression_ctx *ctx, struct iaa_req *req,
			struct idxd_wq *wq,
			dma_addr_t src_addr, unsigned int slen,
			dma_addr_t dst_addr, unsigned int *dlen)
{
	struct iaa_device *iaa_device;
	struct idxd_desc *idxd_desc = ERR_PTR(-EAGAIN);
	u16 alloc_desc_retries = 0;
	struct iax_hw_desc *desc;
	struct idxd_device *idxd;
	struct iaa_wq *iaa_wq;
	struct pci_dev *pdev;
	struct device *dev;
	int ret = 0;

	iaa_wq = idxd_wq_get_private(wq);
	iaa_device = iaa_wq->iaa_device;
	idxd = iaa_device->idxd;
	pdev = idxd->pdev;
	dev = &pdev->dev;

	while ((idxd_desc == ERR_PTR(-EAGAIN)) && (alloc_desc_retries++ < ctx->alloc_comp_desc_timeout)) {
		idxd_desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
		cpu_relax();
	}

	if (IS_ERR(idxd_desc)) {
		dev_dbg(dev, "iaa compress failed: idxd descriptor allocation failure: ret=%ld\n",
			PTR_ERR(idxd_desc));
		return -ENODEV;
	}

	desc = iaa_setup_compress_hw_desc(idxd_desc, src_addr, slen, dst_addr, *dlen,
					  ctx->mode, iaa_device->compression_modes[ctx->mode]);

	if (likely(!ctx->use_irq)) {
		req->drv_data = idxd_desc;
		iaa_submit_desc_movdir64b(wq, idxd_desc);

		/* Update stats */
		update_total_comp_calls();
		update_wq_comp_calls(wq);

		if (req->flags & IAA_REQ_POLL_FLAG)
			return -EINPROGRESS;

		ret = check_completion(dev, idxd_desc->iax_completion, true, false);
		if (ret) {
			dev_dbg(dev, "check_completion failed ret=%d\n", ret);
			goto out;
		}

		*dlen = idxd_desc->iax_completion->output_size;
		req->compression_crc = idxd_desc->iax_completion->crc;

		/* Update stats */
		update_total_comp_bytes_out(*dlen);
		update_wq_comp_bytes(wq, *dlen);
	} else {
		struct acomp_req *areq = req->drv_data;

		desc->flags |= IDXD_OP_FLAG_RCI;

		idxd_desc->crypto.req = areq;
		idxd_desc->crypto.tfm = areq->base.tfm;
		idxd_desc->crypto.src_addr = src_addr;
		idxd_desc->crypto.dst_addr = dst_addr;
		idxd_desc->crypto.compress = true;

		ret = idxd_submit_desc(wq, idxd_desc);
		if (ret) {
			dev_dbg(dev, "submit_desc failed ret=%d\n", ret);
			goto out;
		}

		/* Update stats */
		update_total_comp_calls();
		update_wq_comp_calls(wq);

		return -EINPROGRESS;
	}

out:
	idxd_free_desc(wq, idxd_desc);

	return ret;
}

static int iaa_decompress(struct iaa_compression_ctx *ctx, struct iaa_req *req,
			  struct idxd_wq *wq,
			  dma_addr_t src_addr, unsigned int slen,
			  dma_addr_t dst_addr, unsigned int *dlen)
{
	struct iaa_device *iaa_device;
	struct idxd_desc *idxd_desc = ERR_PTR(-EAGAIN);
	u16 alloc_desc_retries = 0;
	struct iax_hw_desc *desc;
	struct idxd_device *idxd;
	struct iaa_wq *iaa_wq;
	struct pci_dev *pdev;
	struct device *dev;
	int ret = 0;

	iaa_wq = idxd_wq_get_private(wq);
	iaa_device = iaa_wq->iaa_device;
	idxd = iaa_device->idxd;
	pdev = idxd->pdev;
	dev = &pdev->dev;

	while ((idxd_desc == ERR_PTR(-EAGAIN)) && (alloc_desc_retries++ < ctx->alloc_decomp_desc_timeout)) {
		idxd_desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
		cpu_relax();
	}

	if (IS_ERR(idxd_desc)) {
		ret = -ENODEV;
		dev_dbg(dev, "%s: idxd descriptor allocation failed: ret=%ld\n", __func__,
			PTR_ERR(idxd_desc));
		idxd_desc = NULL;
		goto fallback_software_decomp;
	}

	desc = iaa_setup_decompress_hw_desc(idxd_desc, src_addr, slen, dst_addr, *dlen);

	if (likely(!ctx->use_irq)) {
		req->drv_data = idxd_desc;
		iaa_submit_desc_movdir64b(wq, idxd_desc);

		/* Update stats */
		update_total_decomp_calls();
		update_wq_decomp_calls(wq);

		if (req->flags & IAA_REQ_POLL_FLAG)
			return -EINPROGRESS;

		ret = check_completion(dev, idxd_desc->iax_completion, false, false);
	} else {
		struct acomp_req *areq = req->drv_data;

		desc->flags |= IDXD_OP_FLAG_RCI;

		idxd_desc->crypto.req = areq;
		idxd_desc->crypto.tfm = areq->base.tfm;
		idxd_desc->crypto.src_addr = src_addr;
		idxd_desc->crypto.dst_addr = dst_addr;
		idxd_desc->crypto.compress = false;

		ret = idxd_submit_desc(wq, idxd_desc);
		if (ret) {
			dev_dbg(dev, "submit_desc failed ret=%d\n", ret);
			goto fallback_software_decomp;
		}

		/* Update stats */
		update_total_decomp_calls();
		update_wq_decomp_calls(wq);

		return -EINPROGRESS;
	}

fallback_software_decomp:
	if (ret) {
		dev_dbg(dev, "%s: desc allocation/submission/check_completion failed ret=%d\n", __func__, ret);
		if (idxd_desc && idxd_desc->iax_completion->status == IAA_ANALYTICS_ERROR) {
			pr_warn("%s: falling back to deflate-generic decompress, "
				"analytics error code %x\n", __func__,
				idxd_desc->iax_completion->error_code);
		}

		ret = deflate_generic_decompress(req);

		if (ret) {
			pr_err("%s: iaa decompress failed: deflate-generic fallback error ret=%d\n",
			       __func__, ret);
			goto out;
		}
	} else {
		req->dlen = idxd_desc->iax_completion->output_size;

		/* Update stats */
		update_total_decomp_bytes_in(slen);
		update_wq_decomp_bytes(wq, slen);
	}

	*dlen = req->dlen;

out:
	if (idxd_desc)
		idxd_free_desc(wq, idxd_desc);

	return ret;
}

static int iaa_comp_acompress(struct iaa_compression_ctx *ctx, struct iaa_req *req)
{
	dma_addr_t src_addr, dst_addr;
	int nr_sgs, cpu, ret = 0;
	struct iaa_wq *iaa_wq;
	struct idxd_wq *wq;
	struct device *dev;

	if (!req->src || !req->slen || !req->dst) {
		pr_debug("invalid src/dst, not compressing\n");
		return -EINVAL;
	}

	cpu = get_cpu();
	wq = comp_wq_table_next_wq(cpu);
	put_cpu();

	iaa_wq = wq ? idxd_wq_get_private(wq) : NULL;
	if (unlikely(!iaa_wq || !percpu_ref_tryget(&iaa_wq->ref))) {
		pr_debug("no wq available for cpu=%d\n", cpu);
		return -ENODEV;
	}

	dev = &wq->idxd->pdev->dev;

	nr_sgs = dma_map_sg(dev, req->src, 1, DMA_TO_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "couldn't map src sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		goto out;
	}
	src_addr = sg_dma_address(req->src);

	nr_sgs = dma_map_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "couldn't map dst sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		goto err_map_dst;
	}
	dst_addr = sg_dma_address(req->dst);

	ret = iaa_compress(ctx, req, wq, src_addr, req->slen, dst_addr,
			   &req->dlen);
	if (ret == -EINPROGRESS)
		return ret;

	if (!ret && ctx->verify_compress) {
		ret = iaa_remap_for_verify(dev, iaa_wq, req, &src_addr, &dst_addr);
		if (ret) {
			dev_dbg(dev, "%s: compress verify remap failed ret=%d\n", __func__, ret);
			goto out;
		}

		ret = iaa_compress_verify(ctx, req, wq, src_addr, req->slen,
					  dst_addr, req->dlen);
		if (ret)
			dev_dbg(dev, "asynchronous compress verification failed ret=%d\n", ret);

		dma_unmap_sg(dev, req->dst, 1, DMA_TO_DEVICE);
		dma_unmap_sg(dev, req->src, 1, DMA_FROM_DEVICE);

		goto out;
	}

	if (unlikely(ret))
		dev_dbg(dev, "asynchronous compress failed ret=%d\n", ret);

	dma_unmap_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
err_map_dst:
	dma_unmap_sg(dev, req->src, 1, DMA_TO_DEVICE);
out:
	percpu_ref_put(&iaa_wq->ref);

	return ret;
}

static int iaa_comp_adecompress(struct iaa_compression_ctx *ctx, struct iaa_req *req)
{
	dma_addr_t src_addr, dst_addr;
	int nr_sgs, cpu, ret = 0;
	struct iaa_wq *iaa_wq;
	struct device *dev;
	struct idxd_wq *wq;

	if (!req->src || !req->slen) {
		pr_debug("invalid src, not decompressing\n");
		return -EINVAL;
	}

	cpu = get_cpu();
	wq = decomp_wq_table_next_wq(cpu);
	put_cpu();

	iaa_wq = wq ? idxd_wq_get_private(wq) : NULL;
	if (unlikely(!iaa_wq || !percpu_ref_tryget(&iaa_wq->ref))) {
		pr_debug("no wq available for cpu=%d\n", cpu);
		return deflate_generic_decompress(req);
	}

	dev = &wq->idxd->pdev->dev;

	nr_sgs = dma_map_sg(dev, req->src, 1, DMA_TO_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "couldn't map src sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		goto out;
	}
	src_addr = sg_dma_address(req->src);

	nr_sgs = dma_map_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
	if (unlikely(nr_sgs <= 0 || nr_sgs > 1)) {
		dev_dbg(dev, "couldn't map dst sg for iaa device %d,"
			" wq %d: ret=%d\n", iaa_wq->iaa_device->idxd->id,
			iaa_wq->wq->id, ret);
		ret = -EIO;
		goto err_map_dst;
	}
	dst_addr = sg_dma_address(req->dst);

	ret = iaa_decompress(ctx, req, wq, src_addr, req->slen,
			     dst_addr, &req->dlen);
	if (ret == -EINPROGRESS)
		return ret;

	if (unlikely(ret != 0))
		dev_dbg(dev, "asynchronous decompress failed ret=%d\n", ret);

	dma_unmap_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
err_map_dst:
	dma_unmap_sg(dev, req->src, 1, DMA_TO_DEVICE);
out:
	percpu_ref_put(&iaa_wq->ref);

	return ret;
}

static int iaa_comp_poll(struct iaa_compression_ctx *ctx, struct iaa_req *req)
{
	struct idxd_desc *idxd_desc;
	struct idxd_device *idxd;
	struct iaa_wq *iaa_wq;
	struct pci_dev *pdev;
	struct device *dev;
	struct idxd_wq *wq;
	bool compress_op;
	int ret;

	idxd_desc = req->drv_data;
	if (!idxd_desc)
		return -EAGAIN;

	compress_op = (idxd_desc->iax_hw->opcode == IAX_OPCODE_COMPRESS);
	wq = idxd_desc->wq;
	iaa_wq = idxd_wq_get_private(wq);
	idxd = iaa_wq->iaa_device->idxd;
	pdev = idxd->pdev;
	dev = &pdev->dev;

	ret = check_completion(dev, idxd_desc->iax_completion, compress_op, true);
	if (ret == -EAGAIN)
		return ret;
	if (ret)
		goto out;

	req->dlen = idxd_desc->iax_completion->output_size;

	/* Update stats */
	if (compress_op) {
		update_total_comp_bytes_out(req->dlen);
		update_wq_comp_bytes(wq, req->dlen);
	} else {
		update_total_decomp_bytes_in(req->slen);
		update_wq_decomp_bytes(wq, req->slen);
	}

	if (compress_op && ctx->verify_compress) {
		dma_addr_t src_addr, dst_addr;

		req->compression_crc = idxd_desc->iax_completion->crc;

		dma_sync_sg_for_device(dev, req->dst, 1, DMA_FROM_DEVICE);
		dma_sync_sg_for_device(dev, req->src, 1, DMA_TO_DEVICE);

		src_addr = sg_dma_address(req->src);
		dst_addr = sg_dma_address(req->dst);

		ret = iaa_compress_verify(ctx, req, wq, src_addr, req->slen,
					  dst_addr, req->dlen);
	}

out:
	/* caller doesn't call crypto_wait_req, so no acomp_request_complete() */
	dma_unmap_sg(dev, req->dst, 1, DMA_FROM_DEVICE);
	dma_unmap_sg(dev, req->src, 1, DMA_TO_DEVICE);

	idxd_free_desc(idxd_desc->wq, idxd_desc);
	percpu_ref_put(&iaa_wq->ref);

	return ret;
}

static __always_inline void iaa_set_req_poll(
	struct iaa_req *reqs[],
	int nr_reqs,
	bool set_flag)
{
	int i;

	for (i = 0; i < nr_reqs; ++i) {
		set_flag ? (reqs[i]->flags |= IAA_REQ_POLL_FLAG) :
			   (reqs[i]->flags &= ~IAA_REQ_POLL_FLAG);
	}
}

/**
 * This API provides IAA compress batching functionality for use by swap
 * modules.
 *
 * @ctx:  compression ctx for the requested IAA mode (fixed/dynamic).
 * @parent_req: The "parent" iaa_req that contains SG lists for the batch's
 *              inputs and outputs.
 * @unit_size: The unit size to apply to @parent_req->slen to get the number of
 *             scatterlists it contains.
 *
 * The caller should check the individual sg->lengths in the @parent_req for
 * errors, including incompressible page errors.
 *
 * Returns 0 if all compress requests in the batch complete successfully,
 * -EINVAL otherwise.
 */
static int iaa_comp_acompress_batch(
	struct iaa_compression_ctx *ctx,
	struct iaa_req *parent_req,
	unsigned int unit_size)
{
	struct iaa_batch_ctx *cpu_ctx = raw_cpu_ptr(iaa_batch_ctx);
	int nr_reqs = parent_req->slen / unit_size;
	int errors[IAA_CRYPTO_MAX_BATCH_SIZE];
	int *dlens[IAA_CRYPTO_MAX_BATCH_SIZE];
	bool compressions_done = false;
	struct sg_page_iter sgiter;
	struct scatterlist *sg;
	struct iaa_req **reqs;
	int i, err = 0;

	mutex_lock(&cpu_ctx->mutex);

	reqs = cpu_ctx->reqs;

	__sg_page_iter_start(&sgiter, parent_req->src, nr_reqs,
			     parent_req->src->offset/unit_size);

	for (i = 0; i < nr_reqs; ++i, ++sgiter.sg_pgoffset) {
		sg_set_page(reqs[i]->src, sg_page_iter_page(&sgiter), PAGE_SIZE, 0);
		reqs[i]->slen = PAGE_SIZE;
	}

	for_each_sg(parent_req->dst, sg, nr_reqs, i) {
		sg->length = PAGE_SIZE;
		dlens[i] = &sg->length;
		reqs[i]->dst = sg;
		reqs[i]->dlen = PAGE_SIZE;
	}

	iaa_set_req_poll(reqs, nr_reqs, true);

	/*
	 * Prepare and submit the batch of iaa_reqs to IAA. IAA will process
	 * these compress jobs in parallel.
	 */
	for (i = 0; i < nr_reqs; ++i) {
		errors[i] = iaa_comp_acompress(ctx, reqs[i]);

		if (likely(errors[i] == -EINPROGRESS)) {
			errors[i] = -EAGAIN;
		} else if (unlikely(errors[i])) {
			*dlens[i] = errors[i];
			err = -EINVAL;
		} else {
			*dlens[i] = reqs[i]->dlen;
		}
	}

	/*
	 * Asynchronously poll for and process IAA compress job completions.
	 */
	while (!compressions_done) {
		compressions_done = true;

		for (i = 0; i < nr_reqs; ++i) {
			/*
			 * Skip, if the compression has already completed
			 * successfully or with an error.
			 */
			if (errors[i] != -EAGAIN)
				continue;

			errors[i] = iaa_comp_poll(ctx, reqs[i]);

			if (errors[i]) {
				if (likely(errors[i] == -EAGAIN)) {
					compressions_done = false;
				} else {
					*dlens[i] = errors[i];
					err = -EINVAL;
				}
			} else {
				*dlens[i] = reqs[i]->dlen;
			}
		}
	}

	/*
	 * For the same 'reqs[]' to be usable by
	 * iaa_comp_acompress()/iaa_comp_adecompress(),
	 * clear the IAA_REQ_POLL_FLAG bit on all iaa_reqs.
	 */
	iaa_set_req_poll(reqs, nr_reqs, false);

	mutex_unlock(&cpu_ctx->mutex);
	return err;
}

/**
 * This API provides IAA decompress batching functionality for use by swap
 * modules.
 *
 * @ctx:  compression ctx for the requested IAA mode (fixed/dynamic).
 * @parent_req: The "parent" iaa_req that contains SG lists for the batch's
 *              inputs and outputs.
 * @unit_size: The unit size to apply to @parent_req->dlen to get the number of
 *             scatterlists it contains.
 *
 * The caller should check @parent_req->dst scatterlist's component SG lists'
 * @length for errors and handle @length != PAGE_SIZE.
 *
 * Returns 0 if all decompress requests complete successfully,
 * -EINVAL otherwise.
 */
static int iaa_comp_adecompress_batch(
	struct iaa_compression_ctx *ctx,
	struct iaa_req *parent_req,
	unsigned int unit_size)
{
	struct iaa_batch_ctx *cpu_ctx = raw_cpu_ptr(iaa_batch_ctx);
	int nr_reqs = parent_req->dlen / unit_size;
	int errors[IAA_CRYPTO_MAX_BATCH_SIZE];
	int *dlens[IAA_CRYPTO_MAX_BATCH_SIZE];
	bool decompressions_done = false;
	struct scatterlist *sg;
	struct iaa_req **reqs;
	int i, err = 0;

	mutex_lock(&cpu_ctx->mutex);

	reqs = cpu_ctx->reqs;

	for_each_sg(parent_req->src, sg, nr_reqs, i) {
		reqs[i]->src = sg;
		reqs[i]->slen = sg->length;
	}

	for_each_sg(parent_req->dst, sg, nr_reqs, i) {
		dlens[i] = &sg->length;
		reqs[i]->dst = sg;
		reqs[i]->dlen = PAGE_SIZE;
	}

	iaa_set_req_poll(reqs, nr_reqs, true);

	/*
	 * Prepare and submit the batch of iaa_reqs to IAA. IAA will process
	 * these decompress jobs in parallel.
	 */
	for (i = 0; i < nr_reqs; ++i) {
		errors[i] = iaa_comp_adecompress(ctx, reqs[i]);

		/*
		 * If it failed desc allocation/submission, errors[i] can
		 * be 0 or error value from software decompress.
		 */
		if (likely(errors[i] == -EINPROGRESS)) {
			errors[i] = -EAGAIN;
		} else if (unlikely(errors[i])) {
			*dlens[i] = errors[i];
			err = -EINVAL;
		} else {
			*dlens[i] = reqs[i]->dlen;
		}
	}

	/*
	 * Asynchronously poll for and process IAA decompress job completions.
	 */
	while (!decompressions_done) {
		decompressions_done = true;

		for (i = 0; i < nr_reqs; ++i) {
			/*
			 * Skip, if the decompression has already completed
			 * successfully or with an error.
			 */
			if (errors[i] != -EAGAIN)
				continue;

			errors[i] = iaa_comp_poll(ctx, reqs[i]);

			if (errors[i]) {
				if (likely(errors[i] == -EAGAIN)) {
					decompressions_done = false;
				} else {
					*dlens[i] = errors[i];
					err = -EINVAL;
				}
			} else {
				*dlens[i] = reqs[i]->dlen;
			}
		}
	}

	/*
	 * For the same 'reqs[]' to be usable by
	 * iaa_comp_acompress()/iaa_comp_adecompress(),
	 * clear the IAA_REQ_POLL_FLAG bit on all iaa_reqs.
	 */
	iaa_set_req_poll(reqs, nr_reqs, false);

	mutex_unlock(&cpu_ctx->mutex);
	return err;
}

static void compression_ctx_init(struct iaa_compression_ctx *ctx, enum iaa_mode mode)
{
	ctx->mode = mode;
	ctx->alloc_comp_desc_timeout = IAA_ALLOC_DESC_COMP_TIMEOUT;
	ctx->alloc_decomp_desc_timeout = IAA_ALLOC_DESC_DECOMP_TIMEOUT;
	ctx->verify_compress = iaa_verify_compress;
	ctx->async_mode = async_mode;
	ctx->use_irq = use_irq;
}

/*********************************************
 * Interfaces to crypto_alg and crypto_acomp.
 *********************************************/

static int iaa_comp_acompress_main(struct acomp_req *areq)
{
	struct crypto_tfm *tfm = areq->base.tfm;
	struct iaa_compression_ctx *ctx;
	struct iaa_req req;
	int ret = -ENODEV, idx;

	if (iaa_alg_is_registered(crypto_tfm_alg_driver_name(tfm), &idx)) {
		ctx = iaa_ctx[idx];

		if (likely(areq->slen == areq->unit_size) || !areq->unit_size) {
			acomp_to_iaa(areq, &req, ctx);
			ret = iaa_comp_acompress(ctx, &req);
			iaa_to_acomp(unlikely(ret) ? ret : req.dlen, areq);
		} else {
			acomp_to_iaa(areq, &req, ctx);
			ret = iaa_comp_acompress_batch(ctx, &req, areq->unit_size);
			/* 
			 * Set the acomp_req's dlen to be the first SG list's
			 * compressed length/error value.
			 */
			areq->dlen = req.dst->length;
		}
	}

	return ret;
}

static int iaa_comp_adecompress_main(struct acomp_req *areq)
{
	struct crypto_tfm *tfm = areq->base.tfm;
	struct iaa_compression_ctx *ctx;
	struct iaa_req req;
	int ret = -ENODEV, idx;

	if (iaa_alg_is_registered(crypto_tfm_alg_driver_name(tfm), &idx)) {
		ctx = iaa_ctx[idx];

		if (likely(areq->dlen == areq->unit_size) || !areq->unit_size) {
			acomp_to_iaa(areq, &req, ctx);
			ret = iaa_comp_adecompress(ctx, &req);
			iaa_to_acomp(unlikely(ret) ? ret : req.dlen, areq);
		} else {
			acomp_to_iaa(areq, &req, ctx);
			ret = iaa_comp_adecompress_batch(ctx, &req, areq->unit_size);
			/* 
			 * Set the acomp_req's dlen to be the first SG list's
			 * decompressed length/error value.
			 */
			areq->dlen = req.dst->length;
		}
	}

	return ret;
}

static int iaa_comp_init_fixed(struct crypto_acomp *acomp_tfm)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp_tfm);
	struct iaa_compression_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx = iaa_ctx[IAA_MODE_FIXED];

	return 0;
}

static struct acomp_alg iaa_acomp_fixed_deflate = {
	.init			= iaa_comp_init_fixed,
	.compress		= iaa_comp_acompress_main,
	.decompress		= iaa_comp_adecompress_main,
	.base			= {
		.cra_name		= "deflate",
		.cra_driver_name	= "deflate-iaa",
		.cra_flags		= CRYPTO_ALG_ASYNC,
		.cra_ctxsize		= sizeof(struct iaa_compression_ctx),
		.cra_reqsize		= sizeof(u32),
		.cra_module		= THIS_MODULE,
		.cra_priority		= IAA_ALG_PRIORITY,
	}
};

/*******************************************
 * Implement idxd_device_driver interfaces.
 *******************************************/

static void iaa_unregister_compression_device(void)
{
	unsigned int i;

	atomic_set(&iaa_crypto_enabled, 0);

	for (i = 0; i < IAA_COMP_MODES_MAX; ++i) {
		iaa_mode_registered[i] = false;
		kfree(iaa_ctx[i]);
		iaa_ctx[i] = NULL;
	}

	num_iaa_modes_registered = 0;
}

static int iaa_register_compression_device(void)
{
	struct iaa_compression_mode *mode;
	int i, idx;

	for (i = 0; i < IAA_COMP_MODES_MAX; ++i) {
		iaa_mode_registered[i] = false;
		mode = find_iaa_compression_mode(iaa_compression_mode_names[i], &idx);
		if (mode) {
			iaa_ctx[i] = kmalloc(sizeof(struct iaa_compression_ctx), GFP_KERNEL);
			if (!iaa_ctx[i])
				goto err;

			compression_ctx_init(iaa_ctx[i], (enum iaa_mode)i);
			iaa_mode_registered[i] = true;
		}
	}

	if (iaa_mode_registered[IAA_MODE_FIXED])
		return 0;

	pr_err("%s: IAA_MODE_FIXED is not registered.", __func__);

err:
	iaa_unregister_compression_device();
	return -ENODEV;
}

static int iaa_register_acomp_compression_device(void)
{
	int ret = -ENOMEM;

	ret = crypto_register_acomp(&iaa_acomp_fixed_deflate);
	if (ret) {
		pr_err("deflate algorithm acomp fixed registration failed (%d)\n", ret);
		goto err_fixed;
	}

	return 0;

err_fixed:
	iaa_unregister_compression_device();
	return ret;
}

static void iaa_unregister_acomp_compression_device(void)
{
	atomic_set(&iaa_crypto_enabled, 0);

	if (iaa_mode_registered[IAA_MODE_FIXED])
		crypto_unregister_acomp(&iaa_acomp_fixed_deflate);
}

static int iaa_crypto_probe(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	struct idxd_driver_data *data = idxd->data;
	struct device *dev = &idxd_dev->conf_dev;
	bool first_wq = false;
	int ret = 0;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -ENXIO;

	if (data->type != IDXD_TYPE_IAX)
		return -ENODEV;

	mutex_lock(&iaa_devices_lock);

	mutex_lock(&wq->wq_lock);

	if (idxd_wq_get_private(wq)) {
		mutex_unlock(&wq->wq_lock);
		mutex_unlock(&iaa_devices_lock);
		return -EBUSY;
	}

	if (!idxd_wq_driver_name_match(wq, dev)) {
		dev_dbg(dev, "wq %d.%d driver_name match failed: wq driver_name %s, dev driver name %s\n",
			idxd->id, wq->id, wq->driver_name, dev->driver->name);
		idxd->cmd_status = IDXD_SCMD_WQ_NO_DRV_NAME;
		ret = -ENODEV;
		goto err;
	}

	wq->type = IDXD_WQT_KERNEL;

	ret = idxd_drv_enable_wq(wq);
	if (ret < 0) {
		dev_dbg(dev, "enable wq %d.%d failed: %d\n",
			idxd->id, wq->id, ret);
		ret = -ENXIO;
		goto err;
	}

	if (list_empty(&iaa_devices)) {
		ret = alloc_wq_table(wq->idxd->max_wqs);
		if (ret)
			goto err_alloc;
		first_wq = true;
	}

	ret = save_iaa_wq(wq);
	if (ret)
		goto err_save;

	if (!rebalance_wq_table()) {
		dev_dbg(dev, "%s: IAA rebalancing device wq tables failed\n", __func__);
		goto err_register;
	}
	atomic_set(&iaa_crypto_enabled, 1);

	if (first_wq) {
		ret = iaa_register_compression_device();
		if (ret != 0) {
			dev_dbg(dev, "IAA compression device registration failed\n");
			goto err_register;
		}

		ret = iaa_register_acomp_compression_device();
		if (ret != 0) {
			dev_dbg(dev, "IAA compression device acomp registration failed\n");
			goto err_register;
		}

		if (!rebalance_wq_table()) {
			dev_dbg(dev, "%s: Rerun after registration: IAA rebalancing device wq tables failed\n", __func__);
			goto err_register;
		}
		atomic_set(&iaa_crypto_enabled, 1);

		try_module_get(THIS_MODULE);

		pr_info("iaa_crypto now ENABLED\n");
	}

out:
	mutex_unlock(&wq->wq_lock);
	mutex_unlock(&iaa_devices_lock);

	return ret;

err_register:
	remove_iaa_wq(wq);
	free_iaa_wq(idxd_wq_get_private(wq));
err_save:
	if (first_wq)
		free_wq_tables();
err_alloc:
	idxd_drv_disable_wq(wq);
err:
	wq->type = IDXD_WQT_NONE;

	goto out;
}

static void iaa_crypto_remove(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	struct iaa_wq *iaa_wq;

	atomic_set(&iaa_crypto_enabled, 0);
	idxd_wq_quiesce(wq);

	mutex_lock(&iaa_devices_lock);
	mutex_lock(&wq->wq_lock);

	remove_iaa_wq(wq);

	if (!rebalance_wq_table())
		pr_debug("%s: IAA rebalancing device wq tables failed\n", __func__);

	spin_lock(&idxd->dev_lock);
	iaa_wq = idxd_wq_get_private(wq);
	if (!iaa_wq) {
		spin_unlock(&idxd->dev_lock);
		pr_err("%s: no iaa_wq available to remove\n", __func__);
		goto out;
	}

	/* Drop the initial reference. */
	percpu_ref_kill(&iaa_wq->ref);

	while (!iaa_wq->free)
		cpu_relax();

	__free_iaa_wq(iaa_wq);

	idxd_wq_set_private(wq, NULL);
	spin_unlock(&idxd->dev_lock);

	kfree(iaa_wq);

	idxd_drv_disable_wq(wq);

	if (atomic_read(&nr_iaa) == 0) {
		atomic_set(&iaa_crypto_enabled, 0);
		pkg_global_wqs_dealloc();
		free_wq_tables();
		WARN_ON(!list_empty(&iaa_devices));
		iaa_unregister_acomp_compression_device();
		iaa_unregister_compression_device();
		INIT_LIST_HEAD(&iaa_devices);
		module_put(THIS_MODULE);

		pr_info("iaa_crypto now DISABLED\n");
	} else if (rebalance_wq_table()) {
		atomic_set(&iaa_crypto_enabled, 1);
	} else {
		pr_debug("%s: IAA re-rebalancing device wq tables failed\n", __func__);
	}
out:
	mutex_unlock(&wq->wq_lock);
	mutex_unlock(&iaa_devices_lock);
}

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

static struct idxd_device_driver iaa_crypto_driver = {
	.probe = iaa_crypto_probe,
	.remove = iaa_crypto_remove,
	.name = IDXD_SUBDRIVER_NAME,
	.type = dev_types,
	.desc_complete = iaa_desc_complete,
};

/********************
 * Module init/exit.
 ********************/

static void iaa_batch_ctx_dealloc(void)
{
	int cpu;
	u8 i;

	if (!iaa_batch_ctx)
		return;

	for_each_possible_cpu(cpu) {
		struct iaa_batch_ctx *cpu_ctx = per_cpu_ptr(iaa_batch_ctx, cpu);

		if (cpu_ctx && cpu_ctx->reqs) {
			for (i = 0; i < IAA_CRYPTO_MAX_BATCH_SIZE; ++i)
				kfree(cpu_ctx->reqs[i]);
			kfree(cpu_ctx->reqs);
		}
	}

	free_percpu(iaa_batch_ctx);
}

static int __init iaa_crypto_init_module(void)
{
	int cpu, ret = 0;
	u8 i;

	INIT_LIST_HEAD(&iaa_devices);

	nr_cpus = num_possible_cpus();
	nr_cpus_per_package = topology_num_cores_per_package();
	nr_packages = topology_max_packages();

	/* Software fallback compressor */
	deflate_crypto_acomp = crypto_alloc_acomp("deflate", 0, 0);
	if (IS_ERR_OR_NULL(deflate_crypto_acomp)) {
		ret = -ENODEV;
		goto err_deflate_acomp;
	}

	ret = iaa_aecs_init_fixed();
	if (ret < 0) {
		pr_debug("IAA fixed compression mode init failed\n");
		goto err_aecs_init;
	}

	ret = idxd_driver_register(&iaa_crypto_driver);
	if (ret) {
		pr_debug("IAA wq sub-driver registration failed\n");
		goto err_driver_reg;
	}

	ret = driver_create_file(&iaa_crypto_driver.drv,
				&driver_attr_g_comp_wqs_per_iaa);
	if (ret) {
		pr_debug("IAA g_comp_wqs_per_iaa attr creation failed\n");
		goto err_g_comp_wqs_per_iaa_attr_create;
	}

	ret = driver_create_file(&iaa_crypto_driver.drv,
				 &driver_attr_distribute_decomps);
	if (ret) {
		pr_debug("IAA distribute_decomps attr creation failed\n");
		goto err_distribute_decomps_attr_create;
	}

	ret = driver_create_file(&iaa_crypto_driver.drv,
				 &driver_attr_distribute_comps);
	if (ret) {
		pr_debug("IAA distribute_comps attr creation failed\n");
		goto err_distribute_comps_attr_create;
	}

	ret = driver_create_file(&iaa_crypto_driver.drv,
				 &driver_attr_verify_compress);
	if (ret) {
		pr_debug("IAA verify_compress attr creation failed\n");
		goto err_verify_attr_create;
	}

	ret = driver_create_file(&iaa_crypto_driver.drv,
				 &driver_attr_sync_mode);
	if (ret) {
		pr_debug("IAA sync mode attr creation failed\n");
		goto err_sync_attr_create;
	}

	/* Allocate batching resources for iaa_crypto. */
	iaa_batch_ctx = alloc_percpu_gfp(struct iaa_batch_ctx, GFP_KERNEL | __GFP_ZERO);
	if (!iaa_batch_ctx) {
		pr_debug("Failed to allocate per-cpu iaa_batch_ctx\n");
		goto batch_ctx_fail;
	}

	for_each_possible_cpu(cpu) {
		struct iaa_batch_ctx *cpu_ctx = per_cpu_ptr(iaa_batch_ctx, cpu);
		int nid = cpu_to_node(cpu);

		cpu_ctx->reqs = kcalloc_node(IAA_CRYPTO_MAX_BATCH_SIZE,
					     sizeof(struct iaa_req *),
					     GFP_KERNEL, nid);

		if (!cpu_ctx->reqs)
			goto reqs_fail;

		for (i = 0; i < IAA_CRYPTO_MAX_BATCH_SIZE; ++i) {
			cpu_ctx->reqs[i] = kzalloc_node(sizeof(struct iaa_req),
							GFP_KERNEL, nid);
			if (!cpu_ctx->reqs[i]) {
				pr_debug("Could not alloc iaa_req reqs[%d]\n", i);
				goto reqs_fail;
			}

			sg_init_table(&cpu_ctx->reqs[i]->sg_src, 1);
			cpu_ctx->reqs[i]->src = &cpu_ctx->reqs[i]->sg_src;
		}

		mutex_init(&cpu_ctx->mutex);
	}

	if (iaa_crypto_debugfs_init())
		pr_warn("debugfs init failed, stats not available\n");

	pr_debug("initialized\n");
out:
	return ret;

reqs_fail:
	iaa_batch_ctx_dealloc();
batch_ctx_fail:
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_sync_mode);
err_sync_attr_create:
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_verify_compress);
err_verify_attr_create:
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_distribute_comps);
err_distribute_comps_attr_create:
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_distribute_decomps);
err_distribute_decomps_attr_create:
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_g_comp_wqs_per_iaa);
err_g_comp_wqs_per_iaa_attr_create:
	idxd_driver_unregister(&iaa_crypto_driver);
err_driver_reg:
	iaa_aecs_cleanup_fixed();
err_aecs_init:
	if (!IS_ERR_OR_NULL(deflate_crypto_acomp)) {
		crypto_free_acomp(deflate_crypto_acomp);
		deflate_crypto_acomp = NULL;
	}
err_deflate_acomp:

	goto out;
}

static void __exit iaa_crypto_cleanup_module(void)
{
	iaa_unregister_acomp_compression_device();
	iaa_unregister_compression_device();

	iaa_batch_ctx_dealloc();
	iaa_crypto_debugfs_cleanup();
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_sync_mode);
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_verify_compress);
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_distribute_comps);
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_distribute_decomps);
	driver_remove_file(&iaa_crypto_driver.drv,
			   &driver_attr_g_comp_wqs_per_iaa);
	idxd_driver_unregister(&iaa_crypto_driver);
	iaa_aecs_cleanup_fixed();

	if (!IS_ERR_OR_NULL(deflate_crypto_acomp)) {
		crypto_free_acomp(deflate_crypto_acomp);
		deflate_crypto_acomp = NULL;
	}

	pr_debug("cleaned up\n");
}

MODULE_IMPORT_NS("IDXD");
MODULE_LICENSE("GPL");
MODULE_ALIAS_IDXD_DEVICE(0);
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("IAA Compression Accelerator Crypto Driver");

module_init(iaa_crypto_init_module);
module_exit(iaa_crypto_cleanup_module);
