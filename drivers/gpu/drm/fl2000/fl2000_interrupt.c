// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define INTR_BUFSIZE 1

struct fl2000_intr {
	struct usb_device *usb_dev;
	struct regmap *regmap;
	struct drm_device *drm;
	struct urb *urb;
	u8 *buf;
	dma_addr_t transfer_dma;
	struct work_struct work;
	struct workqueue_struct *work_queue;
	/* Sink events read by anyone, not yet reported to userspace */
	atomic_t events;
};

#define FL2000_SINK_EVENTS (FL2000_VGA_STATUS_HDMI_EVENT | FL2000_VGA_STATUS_MONITOR_EVENT | \
			    FL2000_VGA_STATUS_EDID_EVENT)

/*
 * The event bits of the status register clear on read, and both the work below and the connector
 * detection read it: keep the events from every read until the work reports them.
 */
static int fl2000_intr_read_status(struct fl2000_intr *intr, u32 *status)
{
	int ret;

	ret = fl2000_read_status(intr->regmap, status);
	if (ret)
		return ret;

	atomic_or(*status & FL2000_SINK_EVENTS, &intr->events);

	return 0;
}

/**
 * fl2000_intr_monitor_connected() - does the FL2000 sense a monitor on its monitor input
 * @intr: interrupt context
 *
 * The chip powers the DAC and the PLL only while this is the case. On the boards with both
 * outputs the input follows whichever monitor is connected, so it does not tell VGA from HDMI.
 * While HDMI is being driven the sense is forced on and tells nothing: that counts as not sensing,
 * or an HDMI monitor going away would look like a VGA one showing up, until the CRTC is disabled.
 *
 * Return: 1 if a monitor is sensed, 0 if not, a negative error code if the status cannot be read
 */
int fl2000_intr_monitor_connected(struct fl2000_intr *intr)
{
	u32 status;
	int ret;

	ret = fl2000_intr_read_status(intr, &status);
	if (ret)
		return ret;

	if (fl2000_force_connect_active(intr->regmap))
		return 0;

	return !!(status & FL2000_VGA_STATUS_MONITOR_STATUS);
}

static void fl2000_intr_work(struct work_struct *work)
{
	u32 status, events;
	struct fl2000_intr *intr = container_of(work, struct fl2000_intr, work);

	if (fl2000_intr_read_status(intr, &status))
		return;

	events = atomic_xchg(&intr->events, 0);
	if (events)
		fl2000_drm_hotplug(intr->drm, events);
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000_intr *intr = urb->context;

	ret = fl2000_urb_status(usb_dev, urb->status);
	if (ret) {
		/* Killed on purpose (suspend, unplug) is no reason to complain */
		if (ret != -ENOENT && ret != -ECONNRESET && ret != -ESHUTDOWN)
			dev_err(&usb_dev->dev, "Stopping interrupts\n");
		return;
	}

	/* Processing reads registers over USB, which sleeps: defer it to a work item */
	queue_work(intr->work_queue, &intr->work);

	/* Restart urb. It may be getting poisoned, or the device may have just gone. */
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -EPERM && ret != -ENODEV)
		dev_err(&usb_dev->dev, "URB submission failed (%d)\n", ret);
}

/**
 * fl2000_intr_create() - interrupt processing context creation
 * @usb_dev: FL2000 USB device
 * @regmap: FL2000 registers
 * @drm: DRM device that receives the hotplug events
 *
 * Called when the DRM device is set up. fl2000_intr_start() starts polling the interrupt endpoint.
 *
 * Return: interrupt context, or an ERR_PTR() on failure
 */
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct regmap *regmap,
				       struct drm_device *drm)
{
	int ret;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc;
	struct usb_interface *interface = usb_ifnum_to_if(usb_dev, FL2000_USBIF_INTERRUPT);

	/*
	 * There's only one altsetting (#0) and one endpoint (#3) in the interrupt interface (#2)
	 * but let's try and "find" it anyway.
	 */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot find interrupt endpoint\n");
		return ERR_PTR(ret);
	}

	intr = kzalloc_obj(*intr);
	if (!intr)
		return ERR_PTR(-ENOMEM);

	intr->usb_dev = usb_dev;
	intr->regmap = regmap;
	intr->drm = drm;
	INIT_WORK(&intr->work, &fl2000_intr_work);
	atomic_set(&intr->events, 0);

	ret = -ENOMEM;
	intr->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!intr->urb)
		goto err_destroy;

	intr->buf = usb_alloc_coherent(usb_dev, INTR_BUFSIZE, GFP_KERNEL, &intr->transfer_dma);
	if (!intr->buf)
		goto err_destroy;

	intr->work_queue = alloc_ordered_workqueue("fl2000_interrupt", 0);
	if (!intr->work_queue)
		goto err_destroy;

	/* Interrupt URB configuration is static, including allocated buffer */
	usb_fill_int_urb(intr->urb, usb_dev, usb_rcvintpipe(usb_dev, usb_endpoint_num(desc)),
			 intr->buf, INTR_BUFSIZE, fl2000_intr_completion, intr, desc->bInterval);
	intr->urb->transfer_dma = intr->transfer_dma;
	intr->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return intr;

err_destroy:
	fl2000_intr_destroy(intr);

	return ERR_PTR(ret);
}

/**
 * fl2000_intr_quiesce() - stop reporting hotplug events
 * @intr: interrupt context
 *
 * Called before the DRM device is unplugged: an event raised after that would
 * announce a change on a node that was just removed, and probing the connector
 * would talk to hardware that is already gone.
 */
void fl2000_intr_quiesce(struct fl2000_intr *intr)
{
	usb_poison_urb(intr->urb);
	cancel_work_sync(&intr->work);
}

/**
 * fl2000_intr_suspend() - stop polling the interrupt endpoint for system sleep
 * @intr: interrupt context
 *
 * Unlike fl2000_intr_quiesce(), the URB can be submitted again by fl2000_intr_start().
 */
void fl2000_intr_suspend(struct fl2000_intr *intr)
{
	usb_kill_urb(intr->urb);
	cancel_work_sync(&intr->work);
}

/**
 * fl2000_intr_start() - poll the interrupt endpoint
 * @intr: interrupt context
 *
 * When the display is ready to hear about monitors: its hotplug events must find the DRM device
 * registered, and after system sleep the pipeline restored.
 *
 * Return: 0 on success, the URB submission error otherwise
 */
int fl2000_intr_start(struct fl2000_intr *intr)
{
	int ret;

	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret)
		dev_err(&intr->usb_dev->dev, "URB submission failed (%d)\n", ret);

	return ret;
}

void fl2000_intr_destroy(struct fl2000_intr *intr)
{
	usb_poison_urb(intr->urb);
	cancel_work_sync(&intr->work);
	if (intr->work_queue)
		destroy_workqueue(intr->work_queue);
	usb_free_coherent(intr->usb_dev, INTR_BUFSIZE, intr->buf, intr->transfer_dma);
	usb_free_urb(intr->urb);
	kfree(intr);
}
