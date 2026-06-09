// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#include "ploytec_codec.h"

/**
 * ploytec_encode_s24_3le - Encode 4-channel S24_3LE to 48-byte Ploytec frame
 * @dest: 48-byte destination buffer
 * @src: 12-byte source buffer (4 channels * 3 bytes)
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
