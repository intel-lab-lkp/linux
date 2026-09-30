// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Topping M62 -- component master for the card's vendor controls.
 *
 * The M62's analogue gains, output volumes and source selectors are not
 * described by the USB Audio Class.  They are reached over a vendor
 * protocol on the card's HID interface, which hid-topping-m62 speaks.
 *
 * This file speaks none of that protocol.  It publishes the sound card to
 * whoever drives the vendor interface, so that the controls are created on
 * the card that plays the audio rather than on a card of their own, and are
 * torn down when either side goes away.  The lifetime rules are the
 * component framework's, which is the point: neither driver has to guess at
 * the other's state, and neither has to be told about the other's
 * disconnect.
 *
 * The same shape binds HD-audio to the graphics drivers in
 * sound/hda/core/component.c, with sound as the master there too.
 */

#include <linux/component.h>
#include <linux/device.h>
#include <linux/usb.h>

#include <sound/core.h>

#include "usbaudio.h"
#include "mixer.h"
#include "helper.h"
#include "mixer_topping.h"

/*
 * What the master hands the component at bind time.  It lives in devres on
 * the audio control interface rather than in drvdata, because drvdata on a
 * usb_interface belongs to snd-usb-audio itself.  devres_find(), keyed on
 * the release function, gives it back inside the callbacks, which are
 * handed nothing but a struct device *.
 */
struct topping_master {
	struct device *dev;		/* the audio control interface */
	struct snd_card *card;
	struct usb_mixer_interface *mixer;
};

static void topping_master_release(struct device *dev, void *res)
{
	/*
	 * Storage only.  Taking the master down is a separate devres
	 * action registered after the match array, so that it runs
	 * before it -- see topping_master_teardown() below.
	 */
}

static struct topping_master *topping_get_master(struct device *dev)
{
	return devres_find(dev, topping_master_release, NULL, NULL);
}

/*
 * Which of the registered components is ours.
 *
 * This is only ever called against devices that have registered with
 * component_add(), so it does not have to defend itself against the whole
 * device tree.  What it does have to do is tell this card's vendor
 * function apart from a second M62 on another port.
 *
 * The HID device sits two levels below the USB device:
 *
 *	hid_device  ->  usb_interface  ->  usb_device
 *
 * WHICH interface it is, is the HID driver's business: it registers a
 * component for the vendor interface and for nothing else.  So the test
 * here is one of descent alone and needs no HID symbols in sound/usb --
 * which also keeps this file free of any opinion about the M62's
 * interface numbering.
 */
static int topping_match_component(struct device *dev, void *data)
{
	return dev->parent && dev->parent->parent == data;
}

static int topping_master_bind(struct device *dev)
{
	struct topping_master *tm = topping_get_master(dev);

	/*
	 * Not a bug, and deliberately not a WARN.  devres_release_all()
	 * moves every node off the device before it calls a single
	 * release, so between a failed probe beginning to unwind and the
	 * teardown action running, the context is already gone while the
	 * aggregate is still registered.  A component that arrives in
	 * that window has simply come at the wrong moment; EPROBE_DEFER
	 * says so, and the framework does not log it.
	 */
	if (!tm)
		return -EPROBE_DEFER;

	return component_bind_all(dev, tm->card);
}

/*
 * No topping_get_master() here, deliberately.  This can run while the
 * interface's devres is unwinding, and devres_release_all() moves every
 * node off the device before it calls a single release, so the lookup
 * would come back empty and the unbind would be skipped altogether.
 *
 * The component does not need the card from us in any case: it kept the
 * one it was handed at bind, and takes its controls off that.
 */
static void topping_master_unbind(struct device *dev)
{
	component_unbind_all(dev, NULL);
}

static const struct component_master_ops topping_master_ops = {
	.bind	= topping_master_bind,
	.unbind	= topping_master_unbind,
};

/*
 * Registered as a devres action AFTER component_match_add(), because
 * devres unwinds in reverse: this then runs before the match array is
 * freed, and component_unbind_all() walks that array.
 *
 * Two roads reach it.  Normally topping_private_free() calls it through
 * devm_release_action().  The other is a probe that got as far as
 * creating this mixer and then failed: usb_audio_probe() leaves the
 * card and its mixer list alone in that case, as long as an earlier
 * interface had succeeded, so the mixer would outlive the interface
 * whose devres this is.  Unhooking it here is what keeps a later
 * disconnect from reaching freed storage.
 */
static void topping_master_teardown(void *data)
{
	struct topping_master *tm = data;

	component_master_del(tm->dev, &topping_master_ops);

	if (tm->mixer) {
		tm->mixer->private_data = NULL;
		tm->mixer->private_free = NULL;
	}
}

static void topping_private_free(struct usb_mixer_interface *mixer)
{
	struct topping_master *tm = mixer->private_data;

	if (!tm)
		return;

	/*
	 * Reached from snd_usb_mixer_disconnect(), on an unplug and on an
	 * unbind of the audio interface alike.  The action clears
	 * mixer->private_data on its way through, so nothing is left
	 * pointing at the storage dropped below.
	 */
	devm_release_action(tm->dev, topping_master_teardown, tm);
	devres_destroy(tm->dev, topping_master_release, NULL, NULL);
}

int snd_topping_init(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	struct component_match *match = NULL;
	struct usb_interface *intf;
	struct topping_master *tm;
	struct device *dev;
	int err;

	/*
	 * The master hangs off the audio control interface rather than off
	 * the USB device: component_match_add() allocates the match list
	 * with devm, and on an interface that is released when the interface
	 * is unbound.  On the usb_device it would live until the device
	 * itself was released, and a rebind would stack a second list on top
	 * of the first.
	 */
	intf = usb_ifnum_to_if(chip->dev,
			       get_iface_desc(mixer->hostif)->bInterfaceNumber);
	if (!intf)
		return -ENODEV;
	dev = &intf->dev;

	tm = devres_alloc(topping_master_release, sizeof(*tm), GFP_KERNEL);
	if (!tm)
		return -ENOMEM;
	tm->dev = dev;
	tm->card = chip->card;
	tm->mixer = mixer;
	devres_add(dev, tm);

	mixer->private_data = tm;
	mixer->private_free = topping_private_free;

	component_match_add(dev, &match, topping_match_component,
			    &chip->dev->dev);

	/*
	 * component_match_add() reports a failed allocation by storing
	 * an error pointer rather than by returning, and
	 * component_master_add_with_match() dereferences what it is
	 * given without looking.
	 */
	if (IS_ERR(match)) {
		err = PTR_ERR(match);
		goto err_free;
	}

	/*
	 * This returns 0 with the aggregate merely pending when
	 * hid-topping-m62 has not registered its component yet:
	 * try_to_bring_up_aggregate_device() reports an incomplete set as
	 * "not ready", not as an error.  So the card comes up either way and
	 * grows the vendor controls if and when the other half appears.
	 */
	err = component_master_add_with_match(dev, &topping_master_ops, match);
	if (err < 0)
		goto err_free;

	/* Last, so that devres releases it first -- see the teardown. */
	err = devm_add_action(dev, topping_master_teardown, tm);
	if (err < 0) {
		component_master_del(dev, &topping_master_ops);
		goto err_free;
	}

	return 0;

err_free:
	mixer->private_data = NULL;
	mixer->private_free = NULL;
	devres_destroy(dev, topping_master_release, NULL, NULL);
	usb_audio_err(chip, "Topping: no component master: %d\n", err);
	return err;
}
