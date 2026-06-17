// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *   Ploytec USB Protocol Handling
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#include <linux/delay.h>
#include "ploytec_proto.h"

/**
 * ploytec_encode_s24_3le - Encode 4-channel S24_3LE to 48-byte Ploytec frame
 * @dest: 48-byte destination buffer
 * @src: 12-byte source buffer (4 channels * 3 bytes)
 *
 * Ploytec Bit-Plane Interleaving (Playback):
 * The firmware uses a non-standard "bit-plane" format where bits from different
 * channels are interleaved into the same byte.
 * - Each 48-byte frame contains 8 samples for 2 pairs of channels.
 * - Bytes 0-23: ALSA Channels 1 & 3
 * - Bytes 24-47: ALSA Channels 2 & 4
 * - Within each 24-byte block, bits are grouped by significance:
 *   - [0-7]: Most significant bits
 *   - [8-15]: Middle bits
 *   - [16-23]: Least significant bits
 * - bit 0 of each byte corresponds to the first channel in the pair.
 * - bit 1 of each byte corresponds to the second channel in the pair.
 */
void ploytec_encode_s24_3le(u8 *dest, const u8 *src)
{
	int i;

	/* First 24 bytes: odd channels (ALSA Ch 1 & 3) */
	for (i = 0; i < 8; i++) {
		dest[i] = (((src[2] >> (7 - i)) & 1) << 0) |
			  (((src[8] >> (7 - i)) & 1) << 1);
	}
	for (i = 0; i < 8; i++) {
		dest[8 + i] = (((src[1] >> (7 - i)) & 1) << 0) |
			      (((src[7] >> (7 - i)) & 1) << 1);
	}
	for (i = 0; i < 8; i++) {
		dest[16 + i] = (((src[0] >> (7 - i)) & 1) << 0) |
			       (((src[6] >> (7 - i)) & 1) << 1);
	}

	/* Second 24 bytes: even channels (ALSA Ch 2 & 4) */
	for (i = 0; i < 8; i++) {
		dest[24 + i] = (((src[5] >> (7 - i)) & 1) << 0) |
			       (((src[11] >> (7 - i)) & 1) << 1);
	}
	for (i = 0; i < 8; i++) {
		dest[24 + 8 + i] = (((src[4] >> (7 - i)) & 1) << 0) |
				   (((src[10] >> (7 - i)) & 1) << 1);
	}
	for (i = 0; i < 8; i++) {
		dest[24 + 16 + i] = (((src[3] >> (7 - i)) & 1) << 0) |
				    (((src[9] >> (7 - i)) & 1) << 1);
	}
}

/**
 * ploytec_decode_s24_3le - Decode 64-byte Ploytec frame to 6-channel S24_3LE
 * @dest: 18-byte destination buffer (6 channels * 3 bytes)
 * @src: 64-byte source buffer
 *
 * Ploytec Bit-Plane Interleaving (Capture):
 * Similar to encoding, the capture path interleaves 3 pairs of channels
 * into bit-planes (bit 0, 1, and 2 of each byte).
 * - Bytes 0x00-0x17: Pair 1 (bits 0,1,2)
 * - Bytes 0x20-0x37: Pair 2 (bits 0,1,2)
 */
void ploytec_decode_s24_3le(u8 *dest, const u8 *src)
{
	int i;

	/* Channel 1: odd channel 1 (bit 0 of bytes 0x00-0x17) */
	dest[0x00] = 0;
	dest[0x01] = 0;
	dest[0x02] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x00] |= ((src[0x10 + i] & 0x01) << (7 - i));
		dest[0x01] |= ((src[0x08 + i] & 0x01) << (7 - i));
		dest[0x02] |= ((src[0x00 + i] & 0x01) << (7 - i));
	}

	/* Channel 2: even channel 2 (bit 0 of bytes 0x20-0x37) */
	dest[0x03] = 0;
	dest[0x04] = 0;
	dest[0x05] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x03] |= ((src[0x30 + i] & 0x01) << (7 - i));
		dest[0x04] |= ((src[0x28 + i] & 0x01) << (7 - i));
		dest[0x05] |= ((src[0x20 + i] & 0x01) << (7 - i));
	}

	/* Channel 3: odd channel 3 (bit 1 of bytes 0x00-0x17) */
	dest[0x06] = 0;
	dest[0x07] = 0;
	dest[0x08] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x06] |= (((src[0x10 + i] & 0x02) >> 1) << (7 - i));
		dest[0x07] |= (((src[0x08 + i] & 0x02) >> 1) << (7 - i));
		dest[0x08] |= (((src[0x00 + i] & 0x02) >> 1) << (7 - i));
	}

	/* Channel 4: even channel 4 (bit 1 of bytes 0x20-0x37) */
	dest[0x09] = 0;
	dest[0x0A] = 0;
	dest[0x0B] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x09] |= (((src[0x30 + i] & 0x02) >> 1) << (7 - i));
		dest[0x0A] |= (((src[0x28 + i] & 0x02) >> 1) << (7 - i));
		dest[0x0B] |= (((src[0x20 + i] & 0x02) >> 1) << (7 - i));
	}

	/* Channel 5: odd channel 5 (bit 2 of bytes 0x00-0x17) */
	dest[0x0C] = 0;
	dest[0x0D] = 0;
	dest[0x0E] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x0C] |= (((src[0x10 + i] & 0x04) >> 2) << (7 - i));
		dest[0x0D] |= (((src[0x08 + i] & 0x04) >> 2) << (7 - i));
		dest[0x0E] |= (((src[0x00 + i] & 0x04) >> 2) << (7 - i));
	}

	/* Channel 6: even channel 6 (bit 2 of bytes 0x20-0x37) */
	dest[0x0F] = 0;
	dest[0x10] = 0;
	dest[0x11] = 0;
	for (i = 0; i < 8; i++) {
		dest[0x0F] |= (((src[0x30 + i] & 0x04) >> 2) << (7 - i));
		dest[0x10] |= (((src[0x28 + i] & 0x04) >> 2) << (7 - i));
		dest[0x11] |= (((src[0x20 + i] & 0x04) >> 2) << (7 - i));
	}
}

/**
 * ploytec_prepare_out_packet - Prepare a playback packet with default sync/MIDI padding
 * @buf: 512-byte destination buffer
 *
 * Sets the initial pattern: MIDI slot (480) is idle (0xFD), sync byte is
 * set to 0xFF at offset 481, and the padding gap (482-511) is zero-filled.
 */
void ploytec_prepare_out_packet(u8 *buf)
{
	memset(buf, 0, PLOYTEC_PKT_SIZE);
	buf[PLOYTEC_MIDI_OUT_OFFSET] = PLOYTEC_MIDI_IDLE_BYTE;
	buf[PLOYTEC_SYNC_BYTE_OFFSET] = PLOYTEC_SYNC_BYTE_VALUE;
}

/**
 * ploytec_handshake_step - Perform the Ploytec handshake sequence
 * @dev: USB device
 * @xfer_buf: Temporary transfer buffer
 */
int ploytec_handshake_step(struct usb_device *dev, void *xfer_buf)
{
	u8 *buf = xfer_buf;
	u8 status;
	int ret;

	ret = usb_set_interface(dev, 0, 1);
	if (ret < 0)
		return ret;
	msleep(20);

	ret = usb_set_interface(dev, 1, 1);
	if (ret < 0)
		return ret;
	msleep(20);

	/*
	 * Read Firmware (Request 0x56):
	 * This request often fails on the Reloop Jockey3 devices but appears
	 * to be a necessary "poke" that advances the internal state machine.
	 *
	 * TODO: This behavior is currently kept as-is to match observed traces.
	 * There is an opportunity to improve or replace this once we have a
	 * better understanding of the Ploytec firmware interaction through
	 * further protocol analysis or reverse engineering.
	 */
	ret = usb_control_msg_recv(dev, 0, PLOYTEC_REQ_FIRMWARE, 0xC0, 0, 0,
				   buf, 15, 2000, GFP_KERNEL);
	msleep(20);

	/* Read Status (Request 0x49) */
	ret = usb_control_msg_recv(dev, 0, PLOYTEC_REQ_STATUS, 0xC0, 0, 0,
				   buf, 1, 2000, GFP_KERNEL);
	if (ret < 0)
		return ret;

	status = buf[0];
	msleep(20);

	/* Enable device if READY bit is not set */
	if (!(status & PLOYTEC_STATUS_READY)) {
		ret = usb_control_msg_send(dev, 0, PLOYTEC_REQ_STATUS, 0x40,
					   (uint16_t)((status | PLOYTEC_STATUS_READY) & 0xFF),
					   0, NULL, 0, 2000, GFP_KERNEL);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/**
 * ploytec_set_rate - Set hardware sample rate
 * @dev: USB device
 * @xfer_buf: Temporary transfer buffer
 * @rate: Sample rate in Hz
 */
int ploytec_set_rate(struct usb_device *dev, void *xfer_buf, u32 rate)
{
	u8 *buf = xfer_buf;
	int ret;

	buf[0] = rate & 0xFF;
	buf[1] = (rate >> 8) & 0xFF;
	buf[2] = (rate >> 16) & 0xFF;

	/* Set rate on Capture EP 0x86 */
	ret = usb_control_msg_send(dev, 0, PLOYTEC_SET_RATE, PLOYTEC_SET_RATE_VAL,
				   0x0100, 0x0086, buf, 3, 2000, GFP_KERNEL);
	if (ret < 0)
		return ret;

	msleep(50);

	/* Set rate on Playback EP 0x05 */
	ret = usb_control_msg_send(dev, 0, PLOYTEC_SET_RATE, PLOYTEC_SET_RATE_VAL,
				   0x0100, 0x0005, buf, 3, 2000, GFP_KERNEL);
	if (ret < 0)
		return ret;

	msleep(50);
	return 0;
}
