// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Generic MIDI 1.0 Running Status expander
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

/*
 * The logic here is generic MIDI 1.0 Running Status handling: it operates
 * purely on a byte stream, with no dependency on USB or on the rest of this
 * driver. It carries the ploytec_ prefix and lives in the Ploytec layer
 * because that firmware is what requires it -- the device does not accept a
 * Running Status stream and needs every message fully status-prefixed.
 *
 * Should a second driver ever need the same behavior, this would be a
 * reasonable candidate to migrate into a shared kernel library rather than
 * duplicating it.
 */

#include "ploytec_midi.h"

/**
 * ploytec_midi_running_status_expand() - Expand MIDI 1.0 Running Status
 * @state: expander state, zero-initialized by the caller before first use
 * @b: the next raw MIDI byte from the source stream, in stream order
 * @dev: struct device for diagnostic logging (currently unused)
 *
 * MIDI 1.0 allows the status byte of a Channel Voice Message to be omitted
 * when it repeats the previous message's status ("Running Status"). Some
 * MIDI receivers don't implement Running Status and require every message
 * to carry its status byte explicitly; this function expands a Running
 * Status stream back into fully status-prefixed messages.
 *
 * Call this once per raw input byte. Most calls simply return @b unchanged.
 * When a data byte arrives while Running Status is active, the synthesised
 * status byte is returned immediately for the caller to send first, and @b
 * itself is queued in @state->queued_byte (with @state->has_queued_byte set)
 * for the caller to retrieve and send on the next output opportunity, ahead
 * of consuming any further input.
 *
 * Return: the next byte to send to the receiver.
 */
u8 ploytec_midi_running_status_expand(struct ploytec_midi_running_status *state,
				      u8 b, struct device *dev)
{
	u8 byte;

	if (b >= 0x80) { // Status byte
		if (b < 0xf0) { // Channel Voice Message (0x80-0xEF)
			state->running_status = b;
			/* Determine expected data bytes based on MIDI opcode */
			if ((b & 0xf0) == 0xc0 || (b & 0xf0) == 0xd0)
				state->expected_data = 1; // PC, Channel Pressure
			else
				state->expected_data = 2; // Note On/Off, CC, etc.
		} else if (b < 0xf8) { // System Common Message (0xf0-0xf7)
			/* System Common messages clear Running Status */
			state->running_status = 0;
			state->expected_data = 0;
		} else { // System Real-Time Message (0xf8-0xff), do not affect Running Status
			return b;
		}

		state->data_count = state->expected_data; // initialize expected data byte count
		return b;
	}

	/* Data byte */
	if (state->data_count > 0) {
		state->data_count--;
		return b;
	} else if (state->running_status >= 0x80) {
		/* Message is complete but we got a data byte -> expand Running Status */
		byte = state->running_status;
		state->queued_byte = b;
		state->has_queued_byte = true;
		state->data_count = state->expected_data - 1; // already 1 byte queued
		return byte;
	}

	/* No running status expansion active, just send the data byte */
	return b;
}
