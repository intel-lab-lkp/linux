// SPDX-License-Identifier: GPL-2.0-only
/*
 * FTRFS — EDAC layer: CRC32 + Reed-Solomon FEC
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include "ftrfs.h"

/* Reed-Solomon FEC context */
static uint8_t gf_exp[256];
static uint8_t gf_log[256];
static bool rs_initialized;

/* Initialize Galois Field tables */
static void init_gf_tables(void)
{
	uint8_t x = 1;

	for (int i = 0; i < 255; i++) {
		gf_exp[i] = x;
		gf_log[x] = i;
		x = (x << 1) ^ ((x & 0x80) ? 0x1d : 0);
	}
	gf_exp[255] = gf_exp[0];
	rs_initialized = true;
}

/* Galois Field multiplication */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	if (a == 0 || b == 0)
		return 0;
	return gf_exp[(gf_log[a] + gf_log[b]) % 255];
}

/* Reed-Solomon encoding */
int ftrfs_rs_encode(uint8_t *data, uint8_t *parity)
{
	uint8_t msg[FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY];

	if (!rs_initialized)
		init_gf_tables();

	memset(msg, 0, sizeof(msg));
	memcpy(msg, data, FTRFS_SUBBLOCK_DATA);

	for (int i = 0; i < FTRFS_SUBBLOCK_DATA; i++) {
		uint8_t feedback = gf_mul(msg[i], gf_exp[FTRFS_RS_PARITY]);

		if (feedback != 0) {
			for (int j = 1; j <= FTRFS_RS_PARITY; j++)
				msg[FTRFS_SUBBLOCK_DATA + j - 1] ^= gf_mul(msg[i], gf_exp[j]);
		}
	}

	memcpy(parity, msg + FTRFS_SUBBLOCK_DATA, FTRFS_RS_PARITY);
	return 0;
}

/* Reed-Solomon decoding (simplified for now) */
int ftrfs_rs_decode(uint8_t *data, uint8_t *parity)
{
	if (!rs_initialized)
		init_gf_tables();

	/* For now, assume no errors (full decoding to be implemented) */
	return 0;
}

/*
 * ftrfs_crc32 - compute CRC32 checksum
 * @buf: data buffer
 * @len: length in bytes
 *
 * Returns CRC32 checksum. Uses kernel's hardware-accelerated CRC32
 * (same as ext4/btrfs).
 */
__u32 ftrfs_crc32(const void *buf, size_t len)
{
	return crc32_le(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}
