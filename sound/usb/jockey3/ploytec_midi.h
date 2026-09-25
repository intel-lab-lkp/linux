/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Generic MIDI 1.0 Running Status expander
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#ifndef __SOUND_USB_JOCKEY3_PLOYTEC_MIDI_H
#define __SOUND_USB_JOCKEY3_PLOYTEC_MIDI_H

#include <linux/types.h>

struct device;

/**
 * struct ploytec_midi_running_status - MIDI 1.0 Running Status expander state
 * @expected_data: number of data bytes expected for the current @running_status message
 * @data_count: number of data bytes still to be consumed for the in-progress message
 * @running_status: the currently active Running Status (Channel Voice) opcode, or 0 if none
 * @queued_byte: data byte held back when a Running Status byte had to be synthesised
 * @has_queued_byte: true when @queued_byte holds a byte still to be delivered
 *
 * Zero-initialize before first use.
 */
struct ploytec_midi_running_status {
	int expected_data;
	int data_count;
	u8 running_status;
	u8 queued_byte;
	bool has_queued_byte;
};

u8 ploytec_midi_running_status_expand(struct ploytec_midi_running_status *state,
				      u8 b, struct device *dev);

#endif /* __SOUND_USB_JOCKEY3_PLOYTEC_MIDI_H */
