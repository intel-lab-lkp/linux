// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Vendor protocol for Topping interfaces behind a HID channel
 *
 * Copyright (c) 2026 Mikhail Gavrilov <mikhail.v.gavrilov@gmail.com>
 *
 * The M62 (152a:875c) puts its analogue input gains, its output volumes
 * and its output source selectors behind a vendor protocol on a
 * HID-class interface, and exposes none of them through UAC.
 *
 * The protocol was read off the vendor application's traffic.  Frames
 * are fifteen bytes:
 *
 *	22 33 | 20 01 01 | TT | PP | s32 value BE | CRC16 BE | 66 77
 *
 * with TT a target (an input, an output, or the device itself), PP a
 * property of that target, and the checksum CRC-16/MODBUS over bytes
 * 2..10 stored most significant byte first.  Reports arriving from the
 * device are the same frame plus one trailing pad byte; an idle poll
 * returns sixteen zeroes.  The vendor application sends 00 00 in place
 * of the checksum and the device accepts it, so the device evidently
 * does not verify what it receives -- this driver signs its writes
 * anyway, and validates what it reads.
 *
 * The device says nothing until it is subscribed: one write of
 * 0x11/0x24 starts the notification stream, after which every change,
 * including a front panel button, arrives unsolicited.  A second
 * write, 0x11/0x26, makes the device announce what it reports at all.
 *
 * The control pipe is not an option: GET_REPORT and SET_REPORT both
 * stall with EPIPE for every report type, so the interrupt endpoints
 * are the only route.  The report descriptor describes nothing worth
 * having -- a Generic Desktop application collection, eight usages
 * stretched over sixteen unnamed bytes in and out, no report ID -- so
 * this driver takes HID_CONNECT_HIDRAW and no input device.
 *
 * THE CONTROLS ARE NOT THIS DRIVER'S.  They live in snd-usb-audio, on
 * the card that plays the audio, and outlive any binding of this
 * driver.  This driver is the transport: it registers a component, the
 * M62 mixer quirk is the master, and at bind this driver fills in the
 * struct topping_component it is handed with the one operation the
 * controls need -- send a frame -- and from then on passes on every
 * frame the card reports.
 */

#include <linux/cleanup.h>
#include <linux/component.h>
#include <linux/crc16.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <sound/topping.h>

#include "hid-ids.h"

#define TOPPING_FRAME_LEN	15	/* what we send */
#define TOPPING_REPORT_LEN	16	/* what arrives, one pad byte more */

/*
 * The M62 presents two non-audio interfaces.  Interface 3 is
 * Application Specific / DFU and is never touched here.  Interface 4
 * carries the control protocol and is the only one this driver takes.
 */
#define M62_VENDOR_IFNUM	4

/* device-scope properties */
#define TOPPING_TT_DEVICE	0x11
#define TOPPING_PP_SUBSCRIBE	0x24
#define TOPPING_PP_ANNOUNCE	0x26

/*
 * THE SUBSCRIPTION LAPSES.  The vendor application repeats 0x11/0x24
 * every two seconds for as long as it is running, and a device that
 * hears nothing stops reporting -- which is why a listener that
 * subscribed once saw the meters and not much else.  Nothing in the
 * frame says "keep alive"; it is simply the same subscribe again.
 */
#define TOPPING_KEEPALIVE_MS	2000

struct topping_hid {
	struct hid_device *hdev;
	struct usb_interface *intf;	/* for runtime PM */

	/*
	 * The audio side, while it has this driver bound, and NULL
	 * otherwise.  Frames arrive before it is there and after it has
	 * gone, so everything that passes one on reads this under lock.
	 */
	struct topping_component *comp;
	spinlock_t lock;		/* guards comp */

	struct delayed_work keepalive;
};

/* ------------------------------------------------------------------ */
/* the wire								*/
/* ------------------------------------------------------------------ */

static void topping_build(u8 *f, u8 target, u8 prop, s32 value)
{
	u16 crc;

	f[0] = 0x22;
	f[1] = 0x33;
	f[2] = 0x20;
	f[3] = 0x01;
	f[4] = 0x01;
	f[5] = target;
	f[6] = prop;
	put_unaligned_be32(value, f + 7);
	crc = crc16(0xffff, f + 2, 9);
	put_unaligned_be16(crc, f + 11);
	f[13] = 0x66;
	f[14] = 0x77;
}

/*
 * The frame goes out as it is; waking the device is the CALLER's
 * business.  A write asked for by the audio side takes a runtime PM
 * reference first, which wakes what is asleep.  The keepalive and the
 * resume path deliberately do not: the first because a sleeping device
 * has no subscription worth renewing -- resume renews it -- and the
 * second because it IS the resume.
 *
 * That division is also what keeps the keepalive out of a deadlock.
 * If it woke the device itself, a runtime suspend arriving at the same
 * moment would wait in cancel_delayed_work_sync() for a worker that was
 * in turn waiting for that suspend to finish.
 *
 * usbhid drops a leading zero byte, taking it for a report ID this
 * device does not use, and sends the rest on the interrupt OUT
 * endpoint.  So the fifteen bytes that reach the card are the frame
 * and nothing else.
 */
static int topping_send(struct topping_hid *th, u8 target, u8 prop,
			s32 value)
{
	/*
	 * NOIO rather than KERNEL: this is called from the resume path
	 * too, where reclaim can wait on a block device that has not
	 * woken yet.
	 */
	u8 *buf __free(kfree) = kzalloc(TOPPING_REPORT_LEN, GFP_NOIO);
	int err;

	if (!buf)
		return -ENOMEM;

	buf[0] = 0;			/* the report ID usbhid will drop */
	topping_build(buf + 1, target, prop, value);

	err = hid_hw_output_report(th->hdev, buf, TOPPING_FRAME_LEN + 1);
	if (err >= 0)
		return 0;

	/*
	 * A cable pulled out of a running card produces one of these per
	 * write until the disconnect arrives, and none of them says
	 * anything about this driver: ENODEV and ESHUTDOWN are the
	 * device already gone, EPROTO and EILSEQ the bus falling apart
	 * on the way there.  Anything else is worth a line.
	 */
	if (err != -ENODEV && err != -ESHUTDOWN &&
	    err != -EPROTO && err != -EILSEQ)
		hid_err(th->hdev, "write %02x/%02x failed: %d\n",
			target, prop, err);
	return err;
}

/*
 * The one operation the audio side is given.  It wakes the device
 * first; the resume that can run on this very thread as a result only
 * schedules the audio side's write-back, so it takes no lock a caller
 * here may be holding.
 *
 * The write-back after a resume comes through here too, close behind
 * the resume itself, and usbhid allocates its URB with GFP_KERNEL --
 * hence the NOIO scope round the send, for the reason given there.
 */
static int topping_write(struct device *dev, u8 target, u8 prop, s32 value)
{
	struct topping_hid *th = hid_get_drvdata(to_hid_device(dev));
	unsigned int noio;
	int err;

	if (usb_autopm_get_interface(th->intf) < 0)
		return -EIO;

	noio = memalloc_noio_save();
	err = topping_send(th, target, prop, value);
	memalloc_noio_restore(noio);

	usb_autopm_put_interface(th->intf);
	return err;
}

static const struct topping_ops topping_hid_ops = {
	.write	= topping_write,
};

/*
 * What was the URB completion handler, minus everything usbhid now
 * owns: there is no resubmit here and no bus-noise status to sort
 * through.  What is left is the check of the frame.  Which frames
 * mean anything is the audio side's business, so every valid one is
 * passed on, the meters included.
 *
 * Runs in the interrupt handler's context, which is why the audio side
 * is reached under a spinlock.
 */
static int topping_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct topping_hid *th = hid_get_drvdata(hdev);

	if (size < TOPPING_FRAME_LEN)
		return 0;
	if (data[0] != 0x22 || data[1] != 0x33 ||
	    data[13] != 0x66 || data[14] != 0x77)
		return 0;
	if (get_unaligned_be16(data + 11) != crc16(0xffff, data + 2, 9))
		return 0;

	/*
	 * Under the lock that topping_unbind() takes to clear th->comp,
	 * so a frame cannot reach an audio side that has already been
	 * told this driver is going.
	 */
	guard(spinlock_irqsave)(&th->lock);
	if (th->comp)
		th->comp->audio_ops->report(th->comp, data[5], data[6],
					    get_unaligned_be32(data + 7));
	return 0;
}

static void topping_keepalive(struct work_struct *work)
{
	struct topping_hid *th = container_of(work, struct topping_hid,
					      keepalive.work);
	int err;

	err = topping_send(th, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	if (err == -ENODEV || err == -ESHUTDOWN)
		return;		/* the device has gone; nothing to renew */

	schedule_delayed_work(&th->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
}

/* ------------------------------------------------------------------ */
/* component								*/
/* ------------------------------------------------------------------ */

static int topping_bind(struct device *dev, struct device *master,
			void *master_data)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct topping_hid *th = hid_get_drvdata(hdev);
	struct topping_component *comp = master_data;

	comp->dev = &hdev->dev;
	comp->ops = &topping_hid_ops;

	/*
	 * Published only now: until this store every frame stops here,
	 * and from it on every frame reaches the audio side, which keeps
	 * what it is told whether or not its controls exist yet.
	 */
	scoped_guard(spinlock_irqsave, &th->lock)
		th->comp = comp;

	/*
	 * Subscribe and ask for the state HERE rather than at probe:
	 * before this point there is nobody to pass the answer to.  The
	 * card answers in two waves -- the jacks at once, the gain of a
	 * connected input about 5 s later -- so nothing here waits for
	 * them: each value arrives through .raw_event() on its own.
	 */
	topping_send(th, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	topping_send(th, TOPPING_TT_DEVICE, TOPPING_PP_ANNOUNCE, 1);
	schedule_delayed_work(&th->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
	return 0;
}

static void topping_unbind(struct device *dev, struct device *master,
			   void *master_data)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct topping_hid *th = hid_get_drvdata(hdev);
	struct topping_component *comp;

	/*
	 * Taken away under the lock, so that once this scope ends no
	 * frame and no resume can reach the audio side.  Clearing it also
	 * stops topping_resume() from restarting the keepalive, which
	 * makes the cancel below final.
	 *
	 * @master_data is deliberately unused.  The audio side cannot look
	 * its own context up while its devres is unwinding, so it passes
	 * NULL; the structure to clear is the one this driver was handed
	 * at bind, and it is right here.
	 */
	scoped_guard(spinlock_irqsave, &th->lock) {
		comp = th->comp;
		th->comp = NULL;
	}

	if (!comp)
		return;

	cancel_delayed_work_sync(&th->keepalive);

	/*
	 * The audio side stops writing before the operation goes: once
	 * this returns, nothing is inside topping_write() on its behalf
	 * and nothing will enter it.
	 */
	comp->audio_ops->unbound(comp);
	comp->ops = NULL;
	comp->dev = NULL;
}

static const struct component_ops topping_component_ops = {
	.bind	= topping_bind,
	.unbind	= topping_unbind,
};

/* ------------------------------------------------------------------ */
/* HID									*/
/* ------------------------------------------------------------------ */

static int topping_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct topping_hid *th;
	int err;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	th = devm_kzalloc(&hdev->dev, sizeof(*th), GFP_KERNEL);
	if (!th)
		return -ENOMEM;

	th->hdev = hdev;
	th->intf = to_usb_interface(hdev->dev.parent);
	if (th->intf->cur_altsetting->desc.bInterfaceNumber !=
	    M62_VENDOR_IFNUM)
		return -ENODEV;

	spin_lock_init(&th->lock);
	INIT_DELAYED_WORK(&th->keepalive, topping_keepalive);
	hid_set_drvdata(hdev, th);

	err = hid_parse(hdev);
	if (err)
		return err;

	/*
	 * HIDRAW and no input device.  The descriptor would only make a
	 * nonexistent pointer, while a hidraw node is how this protocol
	 * was read in the first place and how the parts not exposed as
	 * controls -- the mixer matrix, the mutes, the EQ -- stay
	 * reachable.
	 */
	err = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (err)
		return err;

	err = hid_hw_open(hdev);
	if (err)
		goto err_stop;

	/*
	 * Reports are dropped for the whole of probe unless this is called,
	 * and component_add() below can bind synchronously when the audio
	 * side is already there -- which subscribes, and the device answers
	 * at once.  Without this the identification wave is thrown away.
	 */
	hid_device_io_start(hdev);

	err = component_add(&hdev->dev, &topping_component_ops);
	if (err)
		goto err_close;

	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return err;
}

static void topping_remove(struct hid_device *hdev)
{
	struct topping_hid *th = hid_get_drvdata(hdev);

	/* Runs topping_unbind() first if the audio side is bound. */
	component_del(&hdev->dev, &topping_component_ops);

	/*
	 * Unconditionally, and after component_del(): if the audio side
	 * had already unbound, the cancel there has been and gone, and
	 * a resume in between could have restarted the work.  This is
	 * the last point before devm frees th, so nothing may outlive
	 * it.
	 */
	cancel_delayed_work_sync(&th->keepalive);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int topping_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct topping_hid *th = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&th->keepalive);
	return 0;
}

static int topping_resume(struct hid_device *hdev)
{
	struct topping_hid *th = hid_get_drvdata(hdev);
	unsigned int noio;

	/*
	 * Nothing to report to yet, and nothing the card needs told:
	 * the next bind does the subscribing.
	 */
	scoped_guard(spinlock_irqsave, &th->lock)
		if (!th->comp)
			return 0;

	/*
	 * Without I/O reclaim: usbhid's own usb_interrupt_msg() allocates
	 * a URB with GFP_KERNEL, so asking for the frame buffer politely
	 * is not enough, and reclaim here can wait on a block device that
	 * has not woken yet.
	 *
	 * Subscribing again is not a formality: the device stops
	 * reporting to a host it has not heard from, and asking for the
	 * state refreshes what may have gone stale while the panel was
	 * reachable and this driver was not.
	 */
	noio = memalloc_noio_save();
	topping_send(th, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	topping_send(th, TOPPING_TT_DEVICE, TOPPING_PP_ANNOUNCE, 1);
	memalloc_noio_restore(noio);

	/*
	 * Tested again under the lock topping_unbind() takes to clear it,
	 * so an unbind racing this function can neither be told of a
	 * resume after it has gone nor leave work behind that nothing
	 * will cancel.  The audio side only schedules its write-back
	 * from here; the writes themselves come later, through
	 * topping_write().
	 */
	scoped_guard(spinlock_irqsave, &th->lock) {
		if (th->comp) {
			th->comp->audio_ops->resumed(th->comp);
			schedule_delayed_work(&th->keepalive,
					      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
		}
	}

	return 0;
}

static const struct hid_device_id topping_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_TOPPING, USB_DEVICE_ID_TOPPING_M62) },
	{ }
};
MODULE_DEVICE_TABLE(hid, topping_devices);

static struct hid_driver topping_driver = {
	.name		= "topping",
	.id_table	= topping_devices,
	.probe		= topping_probe,
	.remove		= topping_remove,
	.raw_event	= topping_raw_event,
	.suspend	= topping_suspend,
	.resume		= topping_resume,
	.reset_resume	= topping_resume,
};
module_hid_driver(topping_driver);

MODULE_DESCRIPTION("Topping vendor control protocol");
MODULE_AUTHOR("Mikhail Gavrilov <mikhail.v.gavrilov@gmail.com>");
MODULE_LICENSE("GPL");
