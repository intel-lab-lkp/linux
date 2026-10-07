// SPDX-License-Identifier: GPL-2.0
/*
 * uio_hv_generic - generic UIO driver for VMBus
 *
 * Copyright (c) 2013-2016 Brocade Communications Systems, Inc.
 * Copyright (c) 2016, Microsoft Corporation.
 *
 * Since the driver does not declare any device ids, you must allocate
 * id and bind the device to the driver yourself.  For example:
 *
 * Associate Network GUID with UIO device
 * # echo "f8615163-df3e-46c5-913f-f2d2f965ed0e" \
 *    > /sys/bus/vmbus/drivers/uio_hv_generic/new_id
 * Then rebind
 * # echo -n "ed963694-e847-4b2a-85af-bc9cfc11d6f3" \
 *    > /sys/bus/vmbus/drivers/hv_netvsc/unbind
 * # echo -n "ed963694-e847-4b2a-85af-bc9cfc11d6f3" \
 *    > /sys/bus/vmbus/drivers/uio_hv_generic/bind
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uio_driver.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/mm.h>
#include <linux/skbuff.h>
#include <linux/hyperv.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "../hv/hyperv_vmbus.h"

#define DRIVER_VERSION	"0.02.1"
#define DRIVER_AUTHOR	"Stephen Hemminger <sthemmin at microsoft.com>"
#define DRIVER_DESC	"Generic UIO driver for VMBus devices"

#define SEND_BUFFER_SIZE (16 * 1024 * 1024)
#define RECV_BUFFER_SIZE (31 * 1024 * 1024)

/*
 * List of resources to be mapped to user space
 * can be extended up to MAX_UIO_MAPS(5) items
 */
enum hv_uio_map {
	TXRX_RING_MAP = 0,
	INT_PAGE_MAP,
	MON_PAGE_MAP,
	RECV_BUF_MAP,
	SEND_BUF_MAP
};

struct hv_uio_private_data {
	struct uio_info info;
	struct hv_device *device;
	atomic_t refcnt;
	struct page *int_pages[1];
	struct page *monitor_pages[1];

	struct vmbus_buffer recv_buffer;
	char	recv_name[32];	/* "recv_4294967295" */

	struct vmbus_buffer send_buffer;
	char	send_name[32];
};

static void set_event(struct vmbus_channel *channel, s32 irq_state)
{
	channel->inbound.ring_buffer->interrupt_mask = !irq_state;
	if (!channel->offermsg.monitor_allocated && irq_state) {
		/* MB is needed for host to see the interrupt mask first */
		virt_mb();
		vmbus_set_event(channel);
	}
}

/*
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 */
static int
hv_uio_irqcontrol(struct uio_info *info, s32 irq_state)
{
	struct hv_uio_private_data *pdata = info->priv;
	struct hv_device *dev = pdata->device;
	struct vmbus_channel *primary, *sc;

	primary = dev->channel;
	set_event(primary, irq_state);

	mutex_lock(&vmbus_connection.channel_mutex);
	list_for_each_entry(sc, &primary->sc_list, sc_list)
		set_event(sc, irq_state);
	mutex_unlock(&vmbus_connection.channel_mutex);

	return 0;
}

/*
 * Callback from vmbus_event when something is in inbound ring.
 */
static void hv_uio_channel_cb(void *context)
{
	struct vmbus_channel *chan = context;
	struct hv_device *hv_dev;
	struct hv_uio_private_data *pdata;

	virt_mb();

	/*
	 * The callback may come from a subchannel, in which case look
	 * for the hv device in the primary channel
	 */
	hv_dev = chan->primary_channel ?
		 chan->primary_channel->device_obj : chan->device_obj;
	pdata = hv_get_drvdata(hv_dev);
	uio_event_notify(&pdata->info);
}

/* Function used for mmap of the ring buffer sysfs interface. */
static bool hv_uio_mmap_range_valid(unsigned long map_pages,
				    pgoff_t offset,
				    unsigned long pages)
{
	return pages && offset < map_pages && pages <= map_pages - offset;
}

struct hv_uio_mmap_region {
	struct page **pages;
	unsigned long page_count;
	struct vmbus_buffer *buffer;
	bool decrypted;
};

static int hv_uio_mmap_get_region(struct hv_uio_private_data *pdata,
				  unsigned int map_index,
				  struct hv_uio_mmap_region *region)
{
	struct hv_device *dev = pdata->device;
	struct vmbus_channel *channel = dev->channel;

	if (map_index >= MAX_UIO_MAPS || !pdata->info.mem[map_index].size)
		return -EINVAL;

	/*
	 * Buffer-backed regions are not described here. Their page array
	 * and protection state are only meaningful as the locked snapshot
	 * from vmbus_buffer_pin_pages(): reading the descriptor outside
	 * that lock can describe a buffer a concurrent release has already
	 * cleared. Only the owning buffer is selected at this stage.
	 */
	region->buffer = NULL;
	region->pages = NULL;
	region->page_count = 0;
	region->decrypted = false;

	switch (map_index) {
	case TXRX_RING_MAP:
		if (channel->state != CHANNEL_OPENED_STATE)
			return -ENODEV;
		region->buffer = &channel->ringbuffer;
		return 0;
	case INT_PAGE_MAP:
		region->pages = pdata->int_pages;
		region->page_count = ARRAY_SIZE(pdata->int_pages);
		region->decrypted = false;
		break;
	case MON_PAGE_MAP:
		region->pages = pdata->monitor_pages;
		region->page_count = ARRAY_SIZE(pdata->monitor_pages);
		region->decrypted = true;
		break;
	case RECV_BUF_MAP:
		region->buffer = &pdata->recv_buffer;
		return 0;
	case SEND_BUF_MAP:
		region->buffer = &pdata->send_buffer;
		return 0;
	default:
		return -EINVAL;
	}

	if (!region->pages || !region->page_count)
		return -EINVAL;

	return 0;
}

static int hv_uio_mmap_prepare_pages(struct vm_area_desc *desc,
				     struct page **pages,
				     unsigned long page_count,
				     pgoff_t offset,
				     bool decrypted)
{
	unsigned long nr_pages = vma_desc_pages(desc);

	if (!vma_desc_test(desc, VMA_SHARED_BIT))
		return -EINVAL;

	if (!pages || !hv_uio_mmap_range_valid(page_count, offset, nr_pages))
		return -EINVAL;

	vma_desc_set_flags(desc, VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT);
	if (decrypted)
		desc->page_prot = pgprot_decrypted(desc->page_prot);

	mmap_action_map_kernel_pages(desc, desc->start, pages + offset,
				     nr_pages);
	return 0;
}

/*
 * mmap_action_map_kernel_pages() only stores the page-array pointer. The
 * mapping takes its own folio references inside insert_pages(), which runs
 * after the prepare callback has returned. Dropping the pin therefore has
 * to wait for the VMA to be established (mapped) or for the attempt to be
 * abandoned. Release is one-shot: the establish and abandon paths can both
 * be reached for the same attempt.
 *
 * Two surfaces, two ownership contracts. /dev/uioN is a character device,
 * so kernfs is not in the path and vm_ops->close is a legal abandon path.
 * The sysfs "ring" bin attribute is kernfs: fs/kernfs/file.c rejects any
 * VMA whose vm_ops carry .close, because kernfs has to wrap the operations
 * and cannot wrap close. That surface installs no vm_ops at all and its
 * synchronous legacy .mmap wrapper owns the pin for the whole window.
 */
#define HV_UIO_PIN_MAGIC 0x48565049UL /* "HVPI" */

struct hv_uio_pin {
	unsigned long magic;
	struct vmbus_buffer_pin snap;
};

static void hv_uio_pin_release(void *data)
{
	struct hv_uio_pin *pin = data;

	if (!pin)
		return;
	/*
	 * close() can be reached with a private_data that this driver did
	 * not install. A foreign pointer must not be freed or unpinned.
	 */
	if (pin->magic != HV_UIO_PIN_MAGIC)
		return;
	pin->magic = 0;
	vmbus_buffer_unpin_pages(&pin->snap);
	kfree(pin);
}

static int hv_uio_pin_vma_mapped(unsigned long start, unsigned long end,
				 pgoff_t pgoff, const struct file *file,
				 void **vm_private_data)
{
	/*
	 * insert_pages() has taken a folio reference for every page the
	 * VMA now maps, so the reclaim worker can no longer free them
	 * under the mapping. The bridge pin is done.
	 */
	hv_uio_pin_release(*vm_private_data);
	*vm_private_data = NULL;
	return 0;
}

static void hv_uio_pin_vma_close(struct vm_area_struct *vma)
{
	/*
	 * Reached with the pin still held when the VMA is torn down before
	 * mapped() ran (an insert_pages() failure, or a merge that never
	 * calls mapped), and with nothing left to do when it already ran.
	 */
	hv_uio_pin_release(vma->vm_private_data);
	vma->vm_private_data = NULL;
}

/*
 * Character-device surface only. The sysfs ring must never adopt this set:
 * see the kernfs contract above.
 */
static const struct vm_operations_struct hv_uio_pin_vm_ops = {
	.mapped = hv_uio_pin_vma_mapped,
	.close = hv_uio_pin_vma_close,
};

/*
 * Install @snap as the VMA's per-mapping state under @ops. Consumes @snap:
 * returns 0 with the pin owned by @desc and @snap cleared, or -ENOMEM with
 * the references dropped here. The caller must not drop the references on
 * the success path: mapped() and close() own them from here.
 */
static int hv_uio_pin_install(struct vm_area_desc *desc,
			      const struct vm_operations_struct *ops,
			      struct vmbus_buffer_pin *snap)
{
	struct hv_uio_pin *pin;

	pin = kzalloc_obj(*pin);
	if (!pin) {
		vmbus_buffer_unpin_pages(snap);
		return -ENOMEM;
	}
	pin->magic = HV_UIO_PIN_MAGIC;
	pin->snap = *snap;
	snap->pages = NULL;
	snap->page_count = 0;

	desc->vm_ops = ops;
	desc->private_data = pin;
	return 0;
}

static int hv_uio_mmap_prepare(struct uio_info *info,
			       struct vm_area_desc *desc)
{
	struct hv_uio_private_data *pdata = info->priv;
	struct hv_uio_mmap_region region;
	struct vmbus_buffer_pin pin = {};
	struct page **pages;
	unsigned long page_count;
	bool decrypted;
	int ret;

	/* UIO encodes the map index in pgoff; mmap offsets start at the map. */
	if (desc->pgoff >= MAX_UIO_MAPS)
		return -EINVAL;

	ret = hv_uio_mmap_get_region(pdata, desc->pgoff, &region);
	if (ret)
		return ret;

	/*
	 * Buffer-backed maps race with the reclaim worker: the page
	 * array and the pages themselves can be freed between the
	 * selection above and the mapping below. Hold references until
	 * the VMA holds its own. The pin is also the only safe source of
	 * the protection state: it is taken in the same critical section
	 * that reads the page array.
	 */
	if (region.buffer) {
		ret = vmbus_buffer_pin_pages(region.buffer, &pin);
		if (ret)
			return ret;
		pages = pin.pages;
		page_count = pin.page_count;
		decrypted = pin.decrypted;
	} else {
		pages = region.pages;
		page_count = region.page_count;
		decrypted = region.decrypted;
	}

	ret = hv_uio_mmap_prepare_pages(desc, pages, page_count, 0, decrypted);
	if (ret) {
		if (region.buffer)
			vmbus_buffer_unpin_pages(&pin);
		return ret;
	}

	if (!region.buffer)
		return 0;

	return hv_uio_pin_install(desc, &hv_uio_pin_vm_ops, &pin);
}

static int
hv_uio_ring_mmap_prepare(struct vmbus_channel *channel,
			 struct vm_area_desc *desc)
{
	struct vmbus_buffer *buffer = &channel->ringbuffer;
	struct vmbus_buffer_pin *pin;
	pgoff_t offset = desc->pgoff;
	int ret;

	if (channel->state != CHANNEL_OPENED_STATE)
		return -ENODEV;

	pin = kzalloc_obj(*pin);
	if (!pin)
		return -ENOMEM;

	ret = vmbus_buffer_pin_pages(buffer, pin);
	if (ret) {
		kfree(pin);
		return ret;
	}

	ret = hv_uio_mmap_prepare_pages(desc, pin->pages, pin->page_count,
					offset, pin->decrypted);
	if (ret) {
		vmbus_buffer_unpin_pages(pin);
		kfree(pin);
		return ret;
	}

	/*
	 * Install no vm_ops. kernfs_fop_mmap() returns -EINVAL for a VMA
	 * whose vm_ops carry .close, and a successful mapping still has to
	 * survive that check, so this surface cannot carry an abandon path
	 * in vm_ops at all. The caller is hv_mmap_ring_buffer_wrapper(),
	 * a synchronous legacy .mmap: it holds @pin across
	 * __compat_vma_mmap() and frees it when that returns, exactly once,
	 * on every outcome. desc->private_data is a plain
	 * struct vmbus_buffer_pin for that wrapper to release.
	 */
	desc->vm_ops = NULL;
	desc->private_data = pin;
	return 0;
}

/* Caller has withdrawn UIO callbacks before removing its backing memory. */
static int hv_uio_disconnect_if_open(struct vmbus_channel *channel,
				     int (*disconnect)(struct vmbus_channel *))
{
	if (channel->state != CHANNEL_OPENED_STATE)
		return 0;

	return disconnect(channel);
}

#if defined(CONFIG_UIO_HV_GENERIC_KUNIT_TEST)
#include "uio_hv_generic_mmap_test.c"
#endif

/* Callback from VMBUS subsystem when new channel created. */
static void
hv_uio_new_channel(struct vmbus_channel *new_sc)
{
	struct hv_device *hv_dev = new_sc->primary_channel->device_obj;
	struct device *device = &hv_dev->device;
	const size_t ring_bytes = SZ_2M;
	int ret;

	/* Create host communication ring */
	ret = vmbus_open(new_sc, ring_bytes, ring_bytes, NULL, 0,
			 hv_uio_channel_cb, new_sc);
	if (ret) {
		dev_err(device, "vmbus_open subchannel failed: %d\n", ret);
		return;
	}

	set_channel_read_mode(new_sc, HV_CALL_ISR);
	ret = hv_create_ring_sysfs(new_sc, hv_uio_ring_mmap_prepare);
	if (ret) {
		dev_err(device, "sysfs create ring bin file failed; %d\n", ret);
		vmbus_close(new_sc);
	}
}

/* free the reserved buffers for send and receive */
static void
hv_uio_cleanup(struct hv_device *dev, struct hv_uio_private_data *pdata)
{
	if (pdata->send_buffer.gpadl.gpadl_handle)
		vmbus_teardown_gpadl_owned(dev->channel, &pdata->send_buffer);
	vmbus_release_buffer(&pdata->send_buffer);

	if (pdata->recv_buffer.gpadl.gpadl_handle)
		vmbus_teardown_gpadl_owned(dev->channel, &pdata->recv_buffer);
	vmbus_release_buffer(&pdata->recv_buffer);
}

/* VMBus primary channel is opened on first use */
static int
hv_uio_open(struct uio_info *info, struct inode *inode)
{
	struct hv_uio_private_data *pdata
		= container_of(info, struct hv_uio_private_data, info);
	struct hv_device *dev = pdata->device;
	int ret;

	if (atomic_inc_return(&pdata->refcnt) != 1)
		return 0;

	vmbus_set_sc_create_callback(dev->channel, hv_uio_new_channel);

	ret = vmbus_connect_ring(dev->channel,
				 hv_uio_channel_cb, dev->channel);
	if (ret)
		atomic_dec(&pdata->refcnt);

	return ret;
}

/* VMBus primary channel is closed on last close */
static int
hv_uio_release(struct uio_info *info, struct inode *inode)
{
	struct hv_uio_private_data *pdata
		= container_of(info, struct hv_uio_private_data, info);
	struct hv_device *dev = pdata->device;
	int ret = 0;

	if (atomic_dec_and_test(&pdata->refcnt))
		ret = vmbus_disconnect_ring(dev->channel);

	return ret;
}

static int
hv_uio_probe(struct hv_device *dev,
	     const struct hv_vmbus_device_id *dev_id)
{
	struct vmbus_channel *channel = dev->channel;
	struct hv_uio_private_data *pdata;
	void *ring_buffer;
	int ret;
	size_t ring_size = hv_dev_ring_size(channel);

	if (!ring_size)
		ring_size = SZ_2M;

	/* Adjust ring size if necessary to have it page aligned */
	ring_size = VMBUS_RING_SIZE(ring_size);

	pdata = devm_kzalloc(&dev->device, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = vmbus_alloc_ring(channel, ring_size, ring_size);
	if (ret)
		return ret;

	set_channel_read_mode(channel, HV_CALL_ISR);

	/* Fill general uio info */
	pdata->info.name = "uio_hv_generic";
	pdata->info.version = DRIVER_VERSION;
	pdata->info.irqcontrol = hv_uio_irqcontrol;
	pdata->info.mmap_prepare = hv_uio_mmap_prepare;
	pdata->info.open = hv_uio_open;
	pdata->info.release = hv_uio_release;
	pdata->info.irq = UIO_IRQ_CUSTOM;
	atomic_set(&pdata->refcnt, 0);

	/* mem resources */
	pdata->info.mem[TXRX_RING_MAP].name = "txrx_rings";
	ring_buffer = channel->ringbuffer.addr;
	pdata->info.mem[TXRX_RING_MAP].addr = (uintptr_t)ring_buffer;
	pdata->info.mem[TXRX_RING_MAP].size
		= channel->ringbuffer_pagecount << PAGE_SHIFT;
	pdata->info.mem[TXRX_RING_MAP].memtype = UIO_MEM_VIRTUAL;

	pdata->info.mem[INT_PAGE_MAP].name = "int_page";
	pdata->info.mem[INT_PAGE_MAP].addr
		= (uintptr_t)vmbus_connection.int_page;
	pdata->info.mem[INT_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata->info.mem[INT_PAGE_MAP].memtype = UIO_MEM_LOGICAL;
	pdata->int_pages[0] = virt_to_page(vmbus_connection.int_page);

	pdata->info.mem[MON_PAGE_MAP].name = "monitor_page";
	pdata->info.mem[MON_PAGE_MAP].addr
		= (uintptr_t)vmbus_connection.monitor_pages[1];
	pdata->info.mem[MON_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata->info.mem[MON_PAGE_MAP].memtype = UIO_MEM_LOGICAL;
	pdata->monitor_pages[0] =
		virt_to_page(vmbus_connection.monitor_pages[1]);

	if (channel->device_id == HV_NIC) {
		ret = vmbus_alloc_buffer_owned(channel, RECV_BUFFER_SIZE, false,
					       &pdata->recv_buffer);
		if (ret)
			goto fail_free_ring;

		ret = vmbus_establish_gpadl_owned(channel, &pdata->recv_buffer);
		if (ret)
			goto fail_close;

		/* put Global Physical Address Label in name */
		snprintf(pdata->recv_name, sizeof(pdata->recv_name),
			 "recv:%u", pdata->recv_buffer.gpadl.gpadl_handle);
		pdata->info.mem[RECV_BUF_MAP].name = pdata->recv_name;
		pdata->info.mem[RECV_BUF_MAP].addr =
			(uintptr_t)pdata->recv_buffer.addr;
		pdata->info.mem[RECV_BUF_MAP].size = RECV_BUFFER_SIZE;
		pdata->info.mem[RECV_BUF_MAP].memtype = UIO_MEM_VIRTUAL;

		ret = vmbus_alloc_buffer_owned(channel, SEND_BUFFER_SIZE, false,
					       &pdata->send_buffer);
		if (ret)
			goto fail_close;

		ret = vmbus_establish_gpadl_owned(channel, &pdata->send_buffer);
		if (ret)
			goto fail_close;

		snprintf(pdata->send_name, sizeof(pdata->send_name),
			 "send:%u", pdata->send_buffer.gpadl.gpadl_handle);
		pdata->info.mem[SEND_BUF_MAP].name = pdata->send_name;
		pdata->info.mem[SEND_BUF_MAP].addr =
			(uintptr_t)pdata->send_buffer.addr;
		pdata->info.mem[SEND_BUF_MAP].size = SEND_BUFFER_SIZE;
		pdata->info.mem[SEND_BUF_MAP].memtype = UIO_MEM_VIRTUAL;
	}

	pdata->info.priv = pdata;
	pdata->device = dev;

	ret = uio_register_device(&dev->device, &pdata->info);
	if (ret) {
		dev_err(&dev->device, "hv_uio register failed\n");
		goto fail_close;
	}

	/*
	 * This internally calls sysfs_update_group, which returns a non-zero value if it executes
	 * before sysfs_create_group. This is expected as the 'ring' will be created later in
	 * vmbus_device_register() -> vmbus_add_channel_kobj(). Thus, no need to check the return
	 * value and print warning.
	 *
	 * Creating/exposing sysfs in driver probe is not encouraged as it can lead to race
	 * conditions with userspace. For backward compatibility, "ring" sysfs could not be removed
	 * or decoupled from uio_hv_generic probe. Userspace programs can make use of inotify
	 * APIs to make sure that ring is created.
	 */
	hv_create_ring_sysfs(channel, hv_uio_ring_mmap_prepare);

	hv_set_drvdata(dev, pdata);

	return 0;

fail_close:
	hv_uio_cleanup(dev, pdata);
fail_free_ring:
	vmbus_free_ring(dev->channel);

	return ret;
}

static void
hv_uio_remove(struct hv_device *dev)
{
	struct hv_uio_private_data *pdata = hv_get_drvdata(dev);
	int ret;

	if (!pdata)
		return;

	hv_remove_ring_sysfs(dev->channel);
	/* Keep event notification alive until channel callbacks are stopped. */
	get_device(&pdata->info.uio_dev->dev);
	uio_unregister_device(&pdata->info);
	/* unregister prevents the eventual fd close from calling .release. */
	ret = hv_uio_disconnect_if_open(dev->channel, vmbus_disconnect_ring);
	if (ret)
		dev_err(&dev->device, "channel disconnect failed: %d\n", ret);
	put_device(&pdata->info.uio_dev->dev);
	hv_uio_cleanup(dev, pdata);

	vmbus_free_ring(dev->channel);
}

static const struct hv_vmbus_device_id hv_uio_id_table[] = {
	{ HV_FCOPY_GUID },
	{}
};
MODULE_DEVICE_TABLE(vmbus, hv_uio_id_table);

static struct hv_driver hv_uio_drv = {
	.name = "uio_hv_generic",
	.id_table = hv_uio_id_table,
	.probe = hv_uio_probe,
	.remove = hv_uio_remove,
};

static int __init
hyperv_module_init(void)
{
	return vmbus_driver_register(&hv_uio_drv);
}

static void __exit
hyperv_module_exit(void)
{
	vmbus_driver_unregister(&hv_uio_drv);
}

module_init(hyperv_module_init);
module_exit(hyperv_module_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
