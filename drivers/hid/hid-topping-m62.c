// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Vendor controls for Topping interfaces behind a HID channel
 *
 * Copyright (c) 2026 Mikhail Gavrilov <mikhail.v.gavrilov@gmail.com>
 *
 * The M62 (152a:875c) puts its analogue input gains and its output
 * volumes behind a vendor protocol on a HID-class interface, and
 * exposes nothing of them through UAC.  What UAC does expose on the
 * capture side is a digital trim AFTER the converter, which cannot buy
 * signal-to-noise: raising it lifts the converter's own floor with the
 * signal.  So the only knob worth automating is unreachable, and a
 * measurement application on Linux has to ask a human to set it by
 * hand on the front panel.
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
 * write, 0x11/0x26, makes the device announce its whole state, which
 * is how the controls are populated without caching what we wrote.
 *
 * The control pipe is not an option: GET_REPORT and SET_REPORT both
 * stall with EPIPE for every report type, so the interrupt endpoints
 * are the only route.  The report descriptor describes nothing worth
 * having -- a Generic Desktop application collection, eight usages
 * stretched over sixteen unnamed bytes in and out, no report ID -- so
 * this driver takes HID_CONNECT_HIDRAW and no input device.
 *
 * THE CONTROLS BELONG ON THE SOUND CARD, so this driver creates no
 * card of its own.  It registers a component; the M62 mixer quirk in
 * snd-usb-audio is the master and hands over its struct snd_card at
 * bind time.  Everything created here is taken off again at unbind,
 * whichever half goes away first, which is what makes the two drivers
 * independent of each other's disconnect.
 */

#include <linux/cleanup.h>
#include <linux/component.h>
#include <linux/crc16.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/tlv.h>

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

/*
 * The two volume tapers, measured against the vendor application's own
 * readout: index 0 is always mute, index 99 always the maximum, the
 * step is 0.5 dB above -10 dB and 1 dB below it, and the family that
 * has to cover 97 dB in 98 steps takes 2 dB below -52 dB as well.
 */
static const DECLARE_TLV_DB_SCALE(topping_tlv_gain, 0, 100, 0);

static const unsigned int topping_tlv_out_9[] = {
	TLV_DB_RANGE_HEAD(4),
	0, 0, SNDRV_CTL_TLVD_DB_SCALE_ITEM(SNDRV_CTL_TLVD_DB_GAIN_MUTE, 0, 1),
	1, 19, SNDRV_CTL_TLVD_DB_SCALE_ITEM(-8800, 200, 0),
	20, 61, SNDRV_CTL_TLVD_DB_SCALE_ITEM(-5100, 100, 0),
	62, 99, SNDRV_CTL_TLVD_DB_SCALE_ITEM(-950, 50, 0),
};

static const unsigned int topping_tlv_out_0[] = {
	TLV_DB_RANGE_HEAD(3),
	0, 0, SNDRV_CTL_TLVD_DB_SCALE_ITEM(SNDRV_CTL_TLVD_DB_GAIN_MUTE, 0, 1),
	1, 79, SNDRV_CTL_TLVD_DB_SCALE_ITEM(-8800, 100, 0),
	80, 99, SNDRV_CTL_TLVD_DB_SCALE_ITEM(-950, 50, 0),
};

/*
 * One row per knob.  A row is the whole description of a control: what
 * to call it, which target and property carry it, the second target
 * that has to be written in step with the first, the range, and the
 * scale.  Adding a knob is adding a row.
 *
 * The outputs come in pairs and the device announces only the second
 * of each pair, so both are written and the second is the one listened
 * for.
 */
struct topping_ctl_desc {
	const char *name;
	u8 target;		/* the target that reports */
	u8 target_pair;		/* written too, or 0 */
	u8 prop;
	int min, max;
	const unsigned int *tlv;
};

static const struct topping_ctl_desc topping_m62_ctls[] = {
	{ "Mic-1 Analog Capture Volume", 0x21, 0, 0x04, 0, 88,
	  topping_tlv_gain },
	{ "Mic-2 Analog Capture Volume", 0x22, 0, 0x04, 0, 88,
	  topping_tlv_gain },
	{ "Aux Capture Volume", 0x23, 0, 0x04, 0, 99,
	  topping_tlv_out_9 },
	{ "Bluetooth Capture Volume", 0x25, 0, 0x04, 0, 99,
	  topping_tlv_out_0 },
	{ "OTG Capture Volume", 0x27, 0, 0x04, 0, 99,
	  topping_tlv_out_0 },
	{ "Headphone Playback Volume", 0x64, 0x63, 0x03, 0, 99,
	  topping_tlv_out_9 },
	{ "OTG Playback Volume", 0x62, 0x61, 0x03, 0, 99,
	  topping_tlv_out_0 },
};

#define TOPPING_NUM_CTLS	ARRAY_SIZE(topping_m62_ctls)

/*
 * WHAT AN OUTPUT CAN LISTEN TO.  The same numbering serves the outputs
 * and the loopback returns, and it has a hole where 4 and 5 would be,
 * so the index of a control item is not the value the card wants and
 * the two are kept side by side.
 *
 * "Unknown" is first and is not a choice: the device NEVER reports a
 * selector, not to us and not to the vendor's own application, which
 * pushes its whole workspace on connect rather than asking.  So a
 * driver cannot learn where an output is pointing, and the only honest
 * thing it can show until a hand has chosen is that it does not know.
 */
static const char * const topping_sources[] = {
	"Unknown", "Mix A", "Mix B", "Mix C", "IN 1", "IN 2", "IN 1+2",
	"AUX", "BT", "OTG IN", "Playback 1/2", "Playback 3/4",
	"Playback 5/6", "Playback 7/8", "Playback 9/10",
};

static const u8 topping_source_value[] = {
	0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
};

struct topping_enum_desc {
	const char *name;
	u8 target;
	u8 prop;
};

/*
 * The selector answers on ONE target of an output's pair, unlike the
 * volume and the mute which must be written to both.
 */
static const struct topping_enum_desc topping_m62_enums[] = {
	{ "Headphone Playback Source", 0x64, 0x02 },
	{ "OTG Playback Source", 0x62, 0x02 },
};

#define TOPPING_NUM_ENUMS	ARRAY_SIZE(topping_m62_enums)
#define TOPPING_NUM_KCTLS	(TOPPING_NUM_CTLS + TOPPING_NUM_ENUMS)

struct topping_m62 {
	struct hid_device *hdev;
	struct usb_interface *intf;	/* for runtime PM */

	/*
	 * NULL until the audio side binds and NULL again after it
	 * unbinds.  Frames arrive before the audio half is there, so
	 * anything that reports to userspace reads this under lock.
	 */
	struct snd_card *card;

	struct delayed_work keepalive;
	struct mutex write_lock;	/* one writer at a time, end to end */
	spinlock_t lock;		/* guards val[] against .raw_event */

	int val[TOPPING_NUM_CTLS];
	int sel[TOPPING_NUM_ENUMS];	/* what a hand chose, or 0 */

	/* the volume controls first, then the selectors */
	struct snd_kcontrol *kctl[TOPPING_NUM_KCTLS];
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
 * business.  A write asked for by a hand takes a runtime PM reference
 * first, which wakes what is asleep.  The keepalive and the resume
 * path deliberately do not: the first because a sleeping device has no
 * subscription worth renewing -- resume renews it -- and the second
 * because it IS the resume.
 *
 * That division is also what keeps the keepalive out of a deadlock.
 * When it woke the device itself, a runtime suspend arriving at the
 * same moment would wait in cancel_delayed_work_sync() for a worker
 * that was in turn waiting for that suspend to finish.
 *
 * usbhid drops a leading zero byte, taking it for a report ID this
 * device does not use, and sends the rest on the interrupt OUT
 * endpoint.  So the fifteen bytes that reach the card are the frame
 * and nothing else.
 */
static int topping_send(struct topping_m62 *m62, u8 target, u8 prop,
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

	err = hid_hw_output_report(m62->hdev, buf, TOPPING_FRAME_LEN + 1);
	if (err >= 0)
		return 0;
	/* ENODEV and ESHUTDOWN are an unplug, not a fault worth a line */
	if (err != -ENODEV && err != -ESHUTDOWN)
		hid_err(m62->hdev, "write %02x/%02x failed: %d\n",
			target, prop, err);
	return err;
}

/* -1 when this frame is not one of ours */
static int topping_index_of(u8 target, u8 prop)
{
	int i;

	for (i = 0; i < TOPPING_NUM_CTLS; i++)
		if (topping_m62_ctls[i].target == target &&
		    topping_m62_ctls[i].prop == prop)
			return i;
	return -1;
}

/*
 * What was the URB completion handler, minus everything usbhid now
 * owns: there is no resubmit here and no bus-noise status to sort
 * through.  What is left is the decode.
 *
 * Runs in the interrupt handler's context, which is why val[] is
 * behind a spinlock rather than the mutex.
 */
static int topping_raw_event(struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct topping_m62 *m62 = hid_get_drvdata(hdev);
	struct snd_kcontrol *kctl = NULL;
	struct snd_card *card;
	int idx, value;

	if (size < TOPPING_FRAME_LEN)
		return 0;
	if (data[0] != 0x22 || data[1] != 0x33 ||
	    data[13] != 0x66 || data[14] != 0x77)
		return 0;
	if (get_unaligned_be16(data + 11) != crc16(0xffff, data + 2, 9))
		return 0;

	idx = topping_index_of(data[5], data[6]);
	if (idx < 0)
		return 0;		/* a meter, or something unnamed */

	value = get_unaligned_be32(data + 7);
	if (value < topping_m62_ctls[idx].min ||
	    value > topping_m62_ctls[idx].max)
		return 0;

	scoped_guard(spinlock_irqsave, &m62->lock) {
		card = m62->card;
		if (m62->val[idx] != value) {
			m62->val[idx] = value;
			kctl = m62->kctl[idx];
		}
	}

	/*
	 * A frame that arrived just as the audio side was unbinding can
	 * notify an id that has already gone.  snd_ctl_notify() takes a
	 * copy of the id and touches nothing that unbind frees, so such
	 * an event is merely wasted.
	 */
	if (card && kctl)
		snd_ctl_notify(card, SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);

	return 0;
}

static void topping_keepalive(struct work_struct *work)
{
	struct topping_m62 *m62 = container_of(work, struct topping_m62,
					       keepalive.work);
	int err;

	err = topping_send(m62, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	if (err == -ENODEV || err == -ESHUTDOWN)
		return;		/* the device has gone; nothing to renew */

	schedule_delayed_work(&m62->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
}

/* ------------------------------------------------------------------ */
/* the volume controls							*/
/* ------------------------------------------------------------------ */

static int topping_ctl_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	const struct topping_ctl_desc *d;

	d = &topping_m62_ctls[kctl->private_value];
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = d->min;
	uinfo->value.integer.max = d->max;
	uinfo->value.integer.step = 1;
	return 0;
}

static int topping_ctl_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct topping_m62 *m62 = snd_kcontrol_chip(kctl);

	guard(spinlock_irqsave)(&m62->lock);
	ucontrol->value.integer.value[0] = m62->val[kctl->private_value];
	return 0;
}

/*
 * Split out so that the runtime PM reference has one place to be
 * dropped.  Entered with the device awake and nothing else held.
 */
static int topping_ctl_write(struct topping_m62 *m62, int idx, int value)
{
	const struct topping_ctl_desc *d = &topping_m62_ctls[idx];
	int prev, err;

	/*
	 * Held from the comparison to the cache update, so that two
	 * writers cannot reach the device in one order and the cache in
	 * the other.
	 */
	guard(mutex)(&m62->write_lock);

	/*
	 * The cache takes the new value BEFORE the write, not after.
	 * The lock cannot be held across a send, and a hand on the front
	 * panel during that window produces a notification .raw_event
	 * stores; updating afterwards would throw that away and leave
	 * the driver claiming a value the device had already moved away
	 * from.  Written first, the device's own report is simply the
	 * last word, which is the right bias.
	 */
	scoped_guard(spinlock_irqsave, &m62->lock) {
		if (m62->val[idx] == value)
			return 0;
		prev = m62->val[idx];
		m62->val[idx] = value;
	}

	err = topping_send(m62, d->target, d->prop, value);
	if (!err && d->target_pair) {
		/*
		 * The device announces only one of a pair, so the other
		 * would drift away unheard.
		 */
		err = topping_send(m62, d->target_pair, d->prop, value);
	}
	if (err < 0) {
		/* put back what was there, unless the device has spoken */
		scoped_guard(spinlock_irqsave, &m62->lock)
			if (m62->val[idx] == value)
				m62->val[idx] = prev;
		return err;
	}

	return 1;
}

static int topping_ctl_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct topping_m62 *m62 = snd_kcontrol_chip(kctl);
	int idx = kctl->private_value;
	int value, err;

	value = ucontrol->value.integer.value[0];
	if (value < topping_m62_ctls[idx].min ||
	    value > topping_m62_ctls[idx].max)
		return -EINVAL;

	/*
	 * THE ORDER OF THESE TWO MATTERS.  Waking the device can run
	 * this driver's own resume callback on this very thread, and
	 * that callback takes write_lock to write the selectors back;
	 * taking write_lock first would meet it already held, by us.
	 *
	 * There is no guard against disconnect here and none is needed:
	 * snd_ctl_remove() in topping_unbind() takes controls_rwsem for
	 * writing, and no control callback can be inside it.
	 */
	if (usb_autopm_get_interface(m62->intf) < 0)
		return -EIO;

	err = topping_ctl_write(m62, idx, value);

	usb_autopm_put_interface(m62->intf);
	return err;
}

static const struct snd_kcontrol_new topping_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = topping_ctl_info,
	.get = topping_ctl_get,
	.put = topping_ctl_put,
};

/* ------------------------------------------------------------------ */
/* the source selectors							*/
/* ------------------------------------------------------------------ */

static int topping_sel_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(topping_sources),
				 topping_sources);
}

static int topping_sel_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct topping_m62 *m62 = snd_kcontrol_chip(kctl);

	guard(mutex)(&m62->write_lock);
	ucontrol->value.enumerated.item[0] = m62->sel[kctl->private_value];
	return 0;
}

static int topping_sel_write(struct topping_m62 *m62, int idx,
			     unsigned int item)
{
	const struct topping_enum_desc *d = &topping_m62_enums[idx];
	int err;

	guard(mutex)(&m62->write_lock);

	/*
	 * "Unknown" is what this control reports until a hand has
	 * chosen, and alsactl stores and restores it like any other
	 * value.  It is not a choice, so writing it changes nothing --
	 * quietly, rather than failing a restore of the driver's own
	 * report.
	 */
	if (!item || m62->sel[idx] == item)
		return 0;

	err = topping_send(m62, d->target, d->prop,
			   topping_source_value[item]);
	if (err < 0)
		return err;

	m62->sel[idx] = item;
	return 1;
}

static int topping_sel_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct topping_m62 *m62 = snd_kcontrol_chip(kctl);
	unsigned int item;
	int err;

	item = ucontrol->value.enumerated.item[0];
	if (item >= ARRAY_SIZE(topping_sources))
		return -EINVAL;

	/* the wake before the lock, for the reason given in _ctl_put */
	if (usb_autopm_get_interface(m62->intf) < 0)
		return -EIO;

	err = topping_sel_write(m62, kctl->private_value, item);

	usb_autopm_put_interface(m62->intf);
	return err;
}

static const struct snd_kcontrol_new topping_sel = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = topping_sel_info,
	.get = topping_sel_get,
	.put = topping_sel_put,
};

/*
 * The gains come back by themselves, since the device announces them,
 * but a selector is never reported: if the card came up on its own
 * defaults while the host slept, this driver's idea of it would be
 * silently wrong, and writing the remembered value would then look
 * like no change at all.  So the choice a hand made is written again.
 */
static void topping_restore_sel(struct topping_m62 *m62)
{
	const struct topping_enum_desc *d;
	int i;

	guard(mutex)(&m62->write_lock);
	for (i = 0; i < TOPPING_NUM_ENUMS; i++) {
		if (!m62->sel[i])
			continue;	/* nothing was ever chosen */
		d = &topping_m62_enums[i];
		topping_send(m62, d->target, d->prop,
			     topping_source_value[m62->sel[i]]);
	}
}

/* ------------------------------------------------------------------ */
/* creating and dropping the controls					*/
/* ------------------------------------------------------------------ */

/*
 * v8 carried the index in a usb_mixer_elem_info, because that is what
 * snd_usb_mixer_add_control() wants.  Off the mixer there is nothing
 * to satisfy: private_data is this driver and private_value is the
 * index, so the per-control allocation goes away with its private_free.
 */
static int topping_add_kctl(struct topping_m62 *m62,
			    const struct snd_kcontrol_new *tmpl,
			    const char *name, int idx, int slot,
			    const unsigned int *tlv)
{
	struct snd_kcontrol *kctl;
	int err;

	kctl = snd_ctl_new1(tmpl, m62);
	if (!kctl)
		return -ENOMEM;

	kctl->private_value = idx;
	kctl->tlv.p = tlv;
	strscpy(kctl->id.name, name, sizeof(kctl->id.name));

	err = snd_ctl_add(m62->card, kctl);
	if (err < 0)
		return err;	/* snd_ctl_add() freed it */

	m62->kctl[slot] = kctl;
	return 0;
}

static void topping_drop_kctls(struct topping_m62 *m62, struct snd_card *card)
{
	int i;

	for (i = 0; i < TOPPING_NUM_KCTLS; i++) {
		snd_ctl_remove(card, m62->kctl[i]);
		m62->kctl[i] = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* component								*/
/* ------------------------------------------------------------------ */

static int topping_bind(struct device *comp, struct device *master,
			void *master_data)
{
	struct hid_device *hdev = to_hid_device(comp);
	struct topping_m62 *m62 = hid_get_drvdata(hdev);
	struct snd_card *card = master_data;
	int i, err;

	scoped_guard(spinlock_irqsave, &m62->lock)
		m62->card = card;

	for (i = 0; i < TOPPING_NUM_CTLS; i++) {
		err = topping_add_kctl(m62, &topping_ctl,
				       topping_m62_ctls[i].name, i, i,
				       topping_m62_ctls[i].tlv);
		if (err < 0)
			goto err_drop;
	}
	for (i = 0; i < TOPPING_NUM_ENUMS; i++) {
		err = topping_add_kctl(m62, &topping_sel,
				       topping_m62_enums[i].name, i,
				       TOPPING_NUM_CTLS + i, NULL);
		if (err < 0)
			goto err_drop;
	}

	/*
	 * Subscribe and ask for the state HERE rather than at probe:
	 * before this point every announced value would land in the
	 * cache with no control to notify.  The device answers in two
	 * waves -- identification at once, the gains about 3.7 s later,
	 * which is the same delay a phantom rail takes to settle -- so
	 * nothing here waits for them: each value lands through
	 * .raw_event and notifies its own control.
	 */
	topping_send(m62, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	topping_send(m62, TOPPING_TT_DEVICE, TOPPING_PP_ANNOUNCE, 1);
	schedule_delayed_work(&m62->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
	return 0;

err_drop:
	topping_drop_kctls(m62, card);
	scoped_guard(spinlock_irqsave, &m62->lock)
		m62->card = NULL;
	return err;
}

static void topping_unbind(struct device *comp, struct device *master,
			   void *master_data)
{
	struct hid_device *hdev = to_hid_device(comp);
	struct topping_m62 *m62 = hid_get_drvdata(hdev);
	struct snd_card *card = master_data;

	cancel_delayed_work_sync(&m62->keepalive);

	/*
	 * Stop reporting before the controls go, so that a frame in
	 * flight cannot look up a control this function is removing.
	 */
	scoped_guard(spinlock_irqsave, &m62->lock)
		m62->card = NULL;

	topping_drop_kctls(m62, card);
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
	struct topping_m62 *m62;
	int err;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	m62 = devm_kzalloc(&hdev->dev, sizeof(*m62), GFP_KERNEL);
	if (!m62)
		return -ENOMEM;

	m62->hdev = hdev;
	m62->intf = to_usb_interface(hdev->dev.parent);
	if (m62->intf->cur_altsetting->desc.bInterfaceNumber !=
	    M62_VENDOR_IFNUM)
		return -ENODEV;

	spin_lock_init(&m62->lock);
	INIT_DELAYED_WORK(&m62->keepalive, topping_keepalive);
	hid_set_drvdata(hdev, m62);

	err = devm_mutex_init(&hdev->dev, &m62->write_lock);
	if (err)
		return err;

	err = hid_parse(hdev);
	if (err)
		return err;

	/*
	 * HIDRAW and no input device.  The descriptor would only make a
	 * nonexistent pointer, while a hidraw node is how this protocol
	 * was read in the first place and how the parts not exposed here
	 * -- the mixer matrix, the mutes, the EQ -- stay reachable.
	 */
	err = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (err)
		return err;

	err = hid_hw_open(hdev);
	if (err)
		goto err_stop;

	/*
	 * XXX awaiting the HID maintainers' word.  usbhid arms every
	 * device it opens for remote wakeup, and this card does not
	 * offer it, which forbids runtime suspend to the whole device.
	 * Nothing here needs it: the resume path subscribes again and
	 * asks the card for its whole state, so a knob turned while the
	 * host slept is picked up by asking rather than by being told.
	 *
	 * This clear does not survive a hidraw open, which calls
	 * hid_hw_open() again -- so if it stays, HID_CONNECT_DRIVER has
	 * to replace HID_CONNECT_HIDRAW above.
	 */
	m62->intf->needs_remote_wakeup = 0;

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
	/* Runs topping_unbind() first if the audio side is bound. */
	component_del(&hdev->dev, &topping_component_ops);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int topping_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct topping_m62 *m62 = hid_get_drvdata(hdev);

	cancel_delayed_work_sync(&m62->keepalive);
	return 0;
}

static int topping_resume(struct hid_device *hdev)
{
	struct topping_m62 *m62 = hid_get_drvdata(hdev);
	unsigned int noio;

	/*
	 * Nothing to report to yet, and nothing the card needs told.
	 * The next bind does the subscribing.
	 */
	if (!READ_ONCE(m62->card))
		return 0;

	/*
	 * Everything below runs without I/O reclaim: usbhid's own
	 * usb_interrupt_msg() allocates a URB with GFP_KERNEL, so asking
	 * for the frame buffer politely is not enough, and reclaim here
	 * can wait on a block device that has not woken yet.
	 *
	 * Subscribing again is not a formality: the device stops
	 * reporting to a host it has not heard from, and asking for the
	 * state refreshes a cache that may have gone stale while the
	 * panel was reachable and this driver was not.
	 */
	noio = memalloc_noio_save();
	topping_send(m62, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	topping_send(m62, TOPPING_TT_DEVICE, TOPPING_PP_ANNOUNCE, 1);
	topping_restore_sel(m62);
	schedule_delayed_work(&m62->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
	memalloc_noio_restore(noio);
	return 0;
}

static const struct hid_device_id topping_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_TOPPING, USB_DEVICE_ID_TOPPING_M62) },
	{ }
};
MODULE_DEVICE_TABLE(hid, topping_devices);

static struct hid_driver topping_driver = {
	.name		= "topping-m62",
	.id_table	= topping_devices,
	.probe		= topping_probe,
	.remove		= topping_remove,
	.raw_event	= topping_raw_event,
	.suspend	= topping_suspend,
	.resume		= topping_resume,
	.reset_resume	= topping_resume,
};
module_hid_driver(topping_driver);

MODULE_DESCRIPTION("Topping M62 vendor controls");
MODULE_AUTHOR("Mikhail Gavrilov <mikhail.v.gavrilov@gmail.com>");
MODULE_LICENSE("GPL");
