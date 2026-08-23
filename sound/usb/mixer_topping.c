// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Mixer controls for Topping interfaces behind a vendor HID channel
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
 * Note that the control pipe is not an option here: GET_REPORT and
 * SET_REPORT both stall with EPIPE for every report type, so the
 * interrupt endpoints on the HID interface are the only route and this
 * driver has to own that interface.  hid_ignore_list keeps usbhid off
 * it; the report descriptor it would bind to describes nothing anyway
 * (a Generic Desktop application collection with eight unnamed usages
 * and no report ID), so no HID functionality is lost.
 */

#include <linux/crc16.h>
#include <linux/unaligned.h>
#include <linux/init.h>
#include <linux/cleanup.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/tlv.h>

#include "usbaudio.h"
#include "mixer.h"
#include "mixer_topping.h"

#define TOPPING_FRAME_LEN	15	/* what we send */
#define TOPPING_REPORT_LEN	16	/* what arrives, one pad byte more */
#define TOPPING_EP_BUF		64	/* the endpoints' packet size */

/* device-scope properties */
#define TOPPING_TT_DEVICE	0x11
#define TOPPING_PP_SUBSCRIBE	0x24
#define TOPPING_PP_ANNOUNCE	0x26

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

/*
 * WHAT AN OUTPUT CAN LISTEN TO. The same numbering serves the outputs
 * and the loopback returns, and it has a hole where 4 and 5 would be,
 * so the index of a control item is not the value the card wants and
 * the two are kept side by side.
 *
 * "Unknown" is first and is not a choice: the device NEVER reports a
 * selector, not to us and not to the vendor's own application, which
 * pushes its whole workspace on connect rather than asking. So a
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

struct topping_mixer {
	struct usb_mixer_interface *mixer;
	struct usb_interface *iface;
	const struct topping_ctl_desc *ctls;
	int num_ctls;
	struct urb *urb;
	u8 *inbuf;
	dma_addr_t inbuf_dma;
	unsigned int pipe_in, pipe_out;
	int interval;
	struct delayed_work keepalive;
	spinlock_t lock;	/* guards val[] against the URB */
	int *val;
	struct snd_kcontrol **kctl;
	int sel[TOPPING_NUM_ENUMS];	/* what a hand chose, or 0 */
};

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

static int topping_send(struct topping_mixer *tm, u8 target, u8 prop,
			s32 value)
{
	u8 *buf __free(kfree) = kzalloc(TOPPING_EP_BUF, GFP_KERNEL);
	int err, actual;

	if (!buf)
		return -ENOMEM;
	topping_build(buf, target, prop, value);
	err = usb_interrupt_msg(tm->mixer->chip->dev, tm->pipe_out,
				buf, TOPPING_FRAME_LEN, &actual, 1000);
	if (err < 0)
		usb_audio_err(tm->mixer->chip,
			      "Topping: write %02x/%02x failed: %d\n",
			      target, prop, err);
	return err;
}

/* -1 when this frame is not one of ours */
static int topping_index_of(struct topping_mixer *tm, u8 target, u8 prop)
{
	int i;

	for (i = 0; i < tm->num_ctls; i++)
		if (tm->ctls[i].target == target && tm->ctls[i].prop == prop)
			return i;
	return -1;
}

static void topping_urb_complete(struct urb *urb)
{
	struct topping_mixer *tm = urb->context;
	const u8 *f = urb->transfer_buffer;
	int idx, value, err;
	bool changed;

	if (urb->status)
		return;		/* resubmitted below only when running */
	if (urb->actual_length < TOPPING_FRAME_LEN)
		goto resubmit;
	if (f[0] != 0x22 || f[1] != 0x33 || f[13] != 0x66 || f[14] != 0x77)
		goto resubmit;
	if (get_unaligned_be16(f + 11) != crc16(0xffff, f + 2, 9))
		goto resubmit;

	idx = topping_index_of(tm, f[5], f[6]);
	if (idx < 0)
		goto resubmit;		/* a meter, or something unnamed */

	value = get_unaligned_be32(f + 7);
	if (value < tm->ctls[idx].min || value > tm->ctls[idx].max)
		goto resubmit;

	changed = false;
	scoped_guard(spinlock_irqsave, &tm->lock) {
		if (tm->val[idx] != value) {
			tm->val[idx] = value;
			changed = true;
		}
	}

	if (changed && tm->kctl[idx])
		snd_ctl_notify(tm->mixer->chip->card,
			       SNDRV_CTL_EVENT_MASK_VALUE,
			       &tm->kctl[idx]->id);

resubmit:
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0 && err != -ENODEV && err != -ESHUTDOWN)
		usb_audio_err(tm->mixer->chip,
			      "Topping: cannot resubmit: %d\n", err);
}

/*
 * THE SUBSCRIPTION LAPSES. The vendor application repeats 0x11/0x24
 * every two seconds for as long as it is running, and a device that
 * hears nothing stops reporting -- which is why a listener that
 * subscribed once saw the meters and not much else. Nothing in the
 * frame says "keep alive"; it is simply the same subscribe again.
 */
#define TOPPING_KEEPALIVE_MS	2000

static void topping_keepalive(struct work_struct *work)
{
	struct topping_mixer *tm = container_of(work, struct topping_mixer,
						keepalive.work);

	topping_send(tm, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	schedule_delayed_work(&tm->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
}

static int topping_ctl_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct topping_mixer *tm = elem->head.mixer->private_data;
	int idx = elem->control;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = tm->ctls[idx].min;
	uinfo->value.integer.max = tm->ctls[idx].max;
	uinfo->value.integer.step = 1;
	return 0;
}

static int topping_ctl_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct topping_mixer *tm = elem->head.mixer->private_data;

	guard(spinlock_irqsave)(&tm->lock);
	ucontrol->value.integer.value[0] = tm->val[elem->control];
	return 0;
}

static int topping_ctl_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct topping_mixer *tm = mixer->private_data;
	const struct topping_ctl_desc *d = &tm->ctls[elem->control];
	int value, err;

	value = ucontrol->value.integer.value[0];
	if (value < d->min || value > d->max)
		return -EINVAL;

	scoped_guard(spinlock_irqsave, &tm->lock)
		if (tm->val[elem->control] == value)
			return 0;

	err = topping_send(tm, d->target, d->prop, value);
	if (err < 0)
		return err;
	if (d->target_pair) {
		/*
		 * The device announces only one of a pair, so the other
		 * would drift away unheard.
		 */
		err = topping_send(tm, d->target_pair, d->prop, value);
		if (err < 0)
			return err;
	}

	scoped_guard(spinlock_irqsave, &tm->lock)
		tm->val[elem->control] = value;
	return 1;
}

static int topping_sel_info(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(topping_sources),
				 topping_sources);
}

static int topping_sel_get(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct topping_mixer *tm = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = tm->sel[elem->control];
	return 0;
}

static int topping_sel_put(struct snd_kcontrol *kctl,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct topping_mixer *tm = elem->head.mixer->private_data;
	const struct topping_enum_desc *d;
	unsigned int item;
	int err;

	item = ucontrol->value.enumerated.item[0];
	if (item >= ARRAY_SIZE(topping_sources))
		return -EINVAL;
	if (!item)
		return -EINVAL;	/* "Unknown" is a report, not a choice */
	if (tm->sel[elem->control] == item)
		return 0;

	d = &topping_m62_enums[elem->control];
	err = topping_send(tm, d->target, d->prop,
			   topping_source_value[item]);
	if (err < 0)
		return err;

	tm->sel[elem->control] = item;
	return 1;
}

static const struct snd_kcontrol_new topping_sel = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = topping_sel_info,
	.get = topping_sel_get,
	.put = topping_sel_put,
};

static const struct snd_kcontrol_new topping_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = topping_ctl_info,
	.get = topping_ctl_get,
	.put = topping_ctl_put,
};

static int topping_add_ctl(struct topping_mixer *tm, int idx)
{
	struct usb_mixer_elem_info *elem;
	struct snd_kcontrol *kctl;
	int err;

	elem = kzalloc_obj(*elem);
	if (!elem)
		return -ENOMEM;

	elem->head.mixer = tm->mixer;
	elem->head.id = 0;
	elem->control = idx;
	elem->channels = 1;
	elem->val_type = USB_MIXER_BESPOKEN;

	kctl = snd_ctl_new1(&topping_ctl, elem);
	if (!kctl) {
		kfree(elem);
		return -ENOMEM;
	}
	kctl->private_free = snd_usb_mixer_elem_free;
	kctl->tlv.p = tm->ctls[idx].tlv;
	strscpy(kctl->id.name, tm->ctls[idx].name, sizeof(kctl->id.name));

	err = snd_usb_mixer_add_control(&elem->head, kctl);
	if (err < 0)
		return err;

	tm->kctl[idx] = kctl;
	return 0;
}

static int topping_add_sel(struct topping_mixer *tm, int idx)
{
	struct usb_mixer_elem_info *elem;
	struct snd_kcontrol *kctl;
	int err;

	elem = kzalloc_obj(*elem);
	if (!elem)
		return -ENOMEM;

	elem->head.mixer = tm->mixer;
	elem->head.id = 0;
	elem->control = idx;
	elem->channels = 1;
	elem->val_type = USB_MIXER_BESPOKEN;

	kctl = snd_ctl_new1(&topping_sel, elem);
	if (!kctl) {
		kfree(elem);
		return -ENOMEM;
	}
	kctl->private_free = snd_usb_mixer_elem_free;
	strscpy(kctl->id.name, topping_m62_enums[idx].name,
		sizeof(kctl->id.name));

	return snd_usb_mixer_add_control(&elem->head, kctl);
}

static void topping_private_free(struct usb_mixer_interface *mixer)
{
	struct topping_mixer *tm = mixer->private_data;

	if (!tm)
		return;
	cancel_delayed_work_sync(&tm->keepalive);
	if (tm->urb) {
		usb_kill_urb(tm->urb);
		usb_free_coherent(mixer->chip->dev, TOPPING_EP_BUF,
				  tm->inbuf, tm->inbuf_dma);
		usb_free_urb(tm->urb);
	}
	kfree(tm->val);
	kfree(tm->kctl);
	kfree(tm);
	mixer->private_data = NULL;
}

/* the HID interface, by class rather than by a number in a comment */
static struct usb_interface *topping_find_iface(struct snd_usb_audio *chip,
						int *ep_in, int *ep_out,
						int *interval)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *alts;
	struct usb_interface *iface;
	int i, e;

	for (i = 0; i < 256; i++) {
		iface = usb_ifnum_to_if(dev, i);
		if (!iface)
			continue;
		alts = &iface->altsetting[0];
		if (alts->desc.bInterfaceClass != USB_CLASS_HID)
			continue;
		*ep_in = *ep_out = 0;
		for (e = 0; e < alts->desc.bNumEndpoints; e++) {
			struct usb_endpoint_descriptor *ep;

			ep = &alts->endpoint[e].desc;
			if (!usb_endpoint_xfer_int(ep))
				continue;
			if (usb_endpoint_dir_in(ep)) {
				*ep_in = usb_endpoint_num(ep);
				*interval = ep->bInterval;
			} else {
				*ep_out = usb_endpoint_num(ep);
			}
		}
		if (*ep_in && *ep_out)
			return iface;
	}
	return NULL;
}

int snd_topping_init(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	struct usb_interface *iface;
	struct topping_mixer *tm;
	int ep_in = 0, ep_out = 0, interval = 5;
	int i, err;

	iface = topping_find_iface(chip, &ep_in, &ep_out, &interval);
	if (!iface) {
		usb_audio_err(chip, "Topping: no vendor HID interface\n");
		return 0;	/* not fatal: the card still plays */
	}
	if (usb_interface_claimed(iface)) {
		usb_audio_err(chip,
			      "Topping: the HID interface is already claimed\n");
		return 0;
	}

	tm = kzalloc_obj(*tm);
	if (!tm)
		return -ENOMEM;

	tm->mixer = mixer;
	tm->iface = iface;
	tm->ctls = topping_m62_ctls;
	tm->num_ctls = ARRAY_SIZE(topping_m62_ctls);
	tm->pipe_in = usb_rcvintpipe(chip->dev, ep_in);
	tm->pipe_out = usb_sndintpipe(chip->dev, ep_out);
	tm->interval = interval;
	spin_lock_init(&tm->lock);
	INIT_DELAYED_WORK(&tm->keepalive, topping_keepalive);

	tm->val = kcalloc(tm->num_ctls, sizeof(*tm->val), GFP_KERNEL);
	tm->kctl = kcalloc(tm->num_ctls, sizeof(*tm->kctl), GFP_KERNEL);
	if (!tm->val || !tm->kctl) {
		err = -ENOMEM;
		goto fail;
	}

	err = snd_usb_claim_iface(chip, iface);
	if (err < 0)
		goto fail;

	tm->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!tm->urb) {
		err = -ENOMEM;
		goto fail;
	}
	tm->inbuf = usb_alloc_coherent(chip->dev, TOPPING_EP_BUF, GFP_KERNEL,
				       &tm->inbuf_dma);
	if (!tm->inbuf) {
		err = -ENOMEM;
		goto fail;
	}
	usb_fill_int_urb(tm->urb, chip->dev, tm->pipe_in,
			 tm->inbuf, TOPPING_EP_BUF,
			 topping_urb_complete, tm, tm->interval);
	tm->urb->transfer_dma = tm->inbuf_dma;
	tm->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	mixer->private_data = tm;
	mixer->private_free = topping_private_free;

	for (i = 0; i < tm->num_ctls; i++) {
		err = topping_add_ctl(tm, i);
		if (err < 0)
			return err;	/* private_free cleans up */
	}
	for (i = 0; i < TOPPING_NUM_ENUMS; i++) {
		err = topping_add_sel(tm, i);
		if (err < 0)
			return err;
	}

	err = usb_submit_urb(tm->urb, GFP_KERNEL);
	if (err < 0) {
		usb_audio_err(chip, "Topping: cannot listen: %d\n", err);
		return err;
	}

	/*
	 * Subscribe, then ask for the state.  The device answers in two
	 * waves -- identification at once, the gains about 3.7 s later,
	 * which is the same delay a phantom rail takes to settle -- so
	 * nothing here waits for them: each value lands through the URB
	 * and notifies its own control.
	 */
	topping_send(tm, TOPPING_TT_DEVICE, TOPPING_PP_SUBSCRIBE, 1);
	topping_send(tm, TOPPING_TT_DEVICE, TOPPING_PP_ANNOUNCE, 1);
	schedule_delayed_work(&tm->keepalive,
			      msecs_to_jiffies(TOPPING_KEEPALIVE_MS));
	return 0;

fail:
	if (tm->inbuf)
		usb_free_coherent(chip->dev, TOPPING_EP_BUF, tm->inbuf,
				  tm->inbuf_dma);
	usb_free_urb(tm->urb);
	kfree(tm->val);
	kfree(tm->kctl);
	kfree(tm);
	return err;
}
