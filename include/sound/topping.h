/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Topping vendor controls -- what hid-topping and the M62 mixer quirk in
 * snd-usb-audio hand each other at component bind.
 *
 * The audio side owns this structure and the controls.  The HID driver,
 * which speaks the card's vendor protocol, fills in @dev and @ops when it
 * binds and clears them when it unbinds, and passes on what the card
 * reports through @audio_ops.  The shape is that of struct
 * drm_audio_component, which joins HD-audio to the graphics drivers.
 */
#ifndef __SOUND_TOPPING_H
#define __SOUND_TOPPING_H

#include <linux/types.h>

struct device;
struct topping_component;

/**
 * struct topping_ops - what the HID driver offers
 * @write: send one frame to the card, waking it first if it is
 *	runtime-suspended.  May sleep.  Returns 0 or a negative error.
 */
struct topping_ops {
	int (*write)(struct device *dev, u8 target, u8 prop, s32 value);
};

/**
 * struct topping_audio_ops - what the audio side asks to be told
 * @report: a valid frame arrived from the card.  Called in atomic context
 *	for every frame, the meters included.
 * @resumed: the card was resumed and subscribed again.  Called in atomic
 *	context.
 * @unbound: the HID driver is unbinding.  @ops goes away when this
 *	returns, so nothing may be inside it by then.  May sleep.
 */
struct topping_audio_ops {
	void (*report)(struct topping_component *comp, u8 target, u8 prop,
		       s32 value);
	void (*resumed)(struct topping_component *comp);
	void (*unbound)(struct topping_component *comp);
};

/**
 * struct topping_component - where the two drivers meet
 * @dev: the HID device, while it is bound
 * @ops: set by the HID driver at bind, cleared at unbind
 * @audio_ops: set by the audio side before the first bind
 */
struct topping_component {
	struct device *dev;
	const struct topping_ops *ops;
	const struct topping_audio_ops *audio_ops;
};

#endif /* __SOUND_TOPPING_H */
