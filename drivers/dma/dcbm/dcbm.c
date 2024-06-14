// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * DMA batch-offlading interface driver
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 */

/*
 * This code exemplifies how to leverage mm layer's migration offload support
 * for batch page offloading using DMA Engine APIs.
 * Developers can use this template to write interface for custom hardware
 * accelerators with specialized capabilities for batch page migration.
 * This interface driver is end-to-end working and can be used for testing the
 * patch series without special hardware given DMAEngine support is available.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/migrate.h>
#include <linux/migrate_dma.h>
#include <linux/printk.h>
#include <linux/sysfs.h>

static struct dma_chan *chan;
static int is_dispatching;

static void folios_copy_dma(struct list_head *dst_list, struct list_head *src_list);
static bool can_migrate_dma(struct folio *dst, struct folio *src);

static DEFINE_MUTEX(migratecfg_mutex);

/* DMA Core Batch Migrator */
struct migrator dmigrator = {
	.name = "DCBM\0",
	.migrate_dma = folios_copy_dma,
	.can_migrate_dma = can_migrate_dma,
	.owner = THIS_MODULE,
};

static ssize_t offloading_set(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	int ccode;
	int action;
	dma_cap_mask_t mask;

	ccode = kstrtoint(buf, 0, &action);
	if (ccode) {
		pr_debug("(%s:) error parsing input %s\n", __func__, buf);
		return ccode;
	}

	/*
	 * action is 0: User wants to disable DMA offloading.
	 * action is 1: User wants to enable DMA offloading.
	 */
	switch (action) {
	case 0:
		mutex_lock(&migratecfg_mutex);
		if (is_dispatching == 1) {
			stop_offloading();
			dma_release_channel(chan);
			is_dispatching = 0;
		} else
			pr_debug("migration offloading is already OFF\n");
		mutex_unlock(&migratecfg_mutex);
		break;
	case 1:
		mutex_lock(&migratecfg_mutex);
		if (is_dispatching == 0) {
			dma_cap_zero(mask);
			dma_cap_set(DMA_MEMCPY, mask);
			chan = dma_request_channel(mask, NULL, NULL);
			if (!chan) {
				chan = ERR_PTR(-ENODEV);
				pr_err("Error requesting DMA channel\n");
				mutex_unlock(&migratecfg_mutex);
				return -ENODEV;
			}
			start_offloading(&dmigrator);
			is_dispatching = 1;
		} else
			pr_debug("migration offloading is already ON\n");
		mutex_unlock(&migratecfg_mutex);
		break;
	default:
		pr_debug("input should be zero or one, parsed as %d\n", action);
	}
	return sizeof(action);
}

static ssize_t offloading_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", is_dispatching);
}

static bool can_migrate_dma(struct folio *dst, struct folio *src)
{
	if (folio_test_hugetlb(src) || folio_test_hugetlb(dst) ||
			folio_has_private(src) || folio_has_private(dst) ||
			(folio_nr_pages(src) != folio_nr_pages(dst)) ||
			folio_nr_pages(src) != 1)
		return false;
	return true;
}

static void folios_copy_dma(struct list_head *dst_list,
		struct list_head *src_list)
{
	int ret = 0;
	struct folio *src, *dst;
	struct dma_device *dev;
	struct device *dma_dev;
	static dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	enum dma_status status;
	enum dma_ctrl_flags flags = DMA_CTRL_ACK;
	dma_addr_t srcdma_handle;
	dma_addr_t dstdma_handle;


	if (!chan) {
		pr_err("error chan uninitialized\n");
		goto fail;
	}
	dev = chan->device;
	if (!dev) {
		pr_err("error dev is NULL\n");
		goto fail;
	}
	dma_dev = dmaengine_get_dma_device(chan);
	if (!dma_dev) {
		pr_err("error dma_dev is NULL\n");
		goto fail;
	}
	dst = list_first_entry(dst_list, struct folio, lru);
	list_for_each_entry(src, src_list, lru) {
		srcdma_handle = dma_map_page(dma_dev, &src->page, 0, 4096, DMA_BIDIRECTIONAL);
		ret = dma_mapping_error(dma_dev, srcdma_handle);
		if (ret) {
			pr_err("src mapping error\n");
			goto fail1;
		}
		dstdma_handle = dma_map_page(dma_dev, &dst->page, 0, 4096, DMA_BIDIRECTIONAL);
		ret = dma_mapping_error(dma_dev, dstdma_handle);
		if (ret) {
			pr_err("dst mapping error\n");
			goto fail2;
		}
		tx = dev->device_prep_dma_memcpy(chan, dstdma_handle, srcdma_handle, 4096, flags);
		if (!tx) {
			ret = -EBUSY;
			pr_err("prep_dma_error\n");
			goto fail3;
		}
		cookie = tx->tx_submit(tx);
		if (dma_submit_error(cookie)) {
			ret = -EINVAL;
			pr_err("dma_submit_error\n");
			goto fail3;
		}
		status = dma_sync_wait(chan, cookie);
		dmaengine_terminate_sync(chan);
		if (status != DMA_COMPLETE) {
			ret = -EINVAL;
			pr_err("error while dma wait\n");
			goto fail3;
		}
fail3:
		dma_unmap_page(dma_dev, dstdma_handle, 4096, DMA_BIDIRECTIONAL);
fail2:
		dma_unmap_page(dma_dev, srcdma_handle, 4096, DMA_BIDIRECTIONAL);
fail1:
		if (ret)
			folio_copy(dst, src);

		dst = list_next_entry(dst, lru);
	}
fail:
	folios_copy(dst_list, src_list);
}

static struct kobject *kobj_ref;
static struct kobj_attribute offloading_attribute = __ATTR(offloading, 0664,
		offloading_show, offloading_set);

static int __init dma_module_init(void)
{
	int ret = 0;

	kobj_ref = kobject_create_and_add("dcbm", kernel_kobj);
	if (!kobj_ref)
		return -ENOMEM;

	ret = sysfs_create_file(kobj_ref, &offloading_attribute.attr);
	if (ret)
		goto out;

	is_dispatching = 0;

	return 0;
out:
	kobject_put(kobj_ref);
	return ret;
}

static void __exit dma_module_exit(void)
{
	/* Stop the DMA offloading to unload the module */

	//sysfs_remove_file(kobj, &offloading_show.attr);
	kobject_put(kobj_ref);
}

module_init(dma_module_init);
module_exit(dma_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shivank Garg");
MODULE_DESCRIPTION("DCBM"); /* DMA Core Batch Migrator */
