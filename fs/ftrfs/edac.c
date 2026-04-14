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

/* Galois Field power */
static uint8_t gf_pow(uint8_t x, int power)
{
	return gf_exp[(gf_log[x] * power) % 255];
}

/* Galois Field inverse */
static uint8_t gf_inv(uint8_t x)
{
	return gf_exp[255 - gf_log[x]];
}

/*
 * Compute RS syndromes.
 * Returns true if all syndromes zero (no error).
 */
static bool rs_calc_syndromes(uint8_t *msg, int msglen, uint8_t *syndromes)
{
	bool all_zero = true;
	int i, j;

	for (i = 0; i < FTRFS_RS_PARITY; i++) {
		syndromes[i] = 0;
		for (j = 0; j < msglen; j++)
			syndromes[i] ^= gf_mul(msg[j], gf_pow(gf_exp[1], i * j));
		if (syndromes[i])
			all_zero = false;
	}
	return all_zero;
}

/*
 * Berlekamp-Massey — find error locator polynomial.
 * Returns number of errors, or -1 if uncorrectable.
 */
static int rs_berlekamp_massey(uint8_t *syndromes, uint8_t *err_loc,
			       int *err_loc_len)
{
	uint8_t old_loc[FTRFS_RS_PARITY + 1];
	uint8_t tmp[FTRFS_RS_PARITY + 1];
	uint8_t new_loc[FTRFS_RS_PARITY + 1];
	int old_len = 1;
	int i, j, nerr, new_len;
	uint8_t delta;

	err_loc[0] = 1;
	*err_loc_len = 1;
	old_loc[0] = 1;

	for (i = 0; i < FTRFS_RS_PARITY; i++) {
		delta = syndromes[i];
		for (j = 1; j < *err_loc_len; j++)
			delta ^= gf_mul(err_loc[j], syndromes[i - j]);

		tmp[0] = 0;
		memcpy(tmp + 1, old_loc, old_len);

		if (delta == 0) {
			old_len++;
		} else if (2 * (*err_loc_len - 1) <= i) {
			new_len = old_len + 1;
			for (j = 0; j < new_len; j++) {
				new_loc[j] = (j < *err_loc_len) ? err_loc[j] : 0;
				new_loc[j] ^= gf_mul(delta, tmp[j]);
			}
			memcpy(old_loc, err_loc, *err_loc_len);
			old_len = *err_loc_len + 1 - new_len + old_len;
			memcpy(err_loc, new_loc, new_len);
			*err_loc_len = new_len;
		} else {
			for (j = 0; j < *err_loc_len; j++)
				err_loc[j] ^= gf_mul(delta, tmp[j]);
			old_len++;
		}
	}

	nerr = *err_loc_len - 1;
	if (nerr > FTRFS_RS_PARITY / 2)
		return -1;
	return nerr;
}

/*
 * Chien search — find roots of error locator polynomial.
 * Returns number of roots found.
 */
static int rs_chien_search(uint8_t *err_loc, int err_loc_len,
			   int msglen, int *errs)
{
	int nerrs = 0;
	int i, j;
	uint8_t val;

	for (i = 0; i < msglen; i++) {
		val = 0;
		for (j = 0; j < err_loc_len; j++)
			val ^= gf_mul(err_loc[j], gf_pow(gf_exp[1], i * j));
		if (val == 0)
			errs[nerrs++] = msglen - 1 - i;
	}
	return nerrs;
}

/*
 * Forney algorithm — compute and apply error corrections.
 */
static void rs_forney(uint8_t *msg, uint8_t *syndromes,
		      uint8_t *err_loc, int err_loc_len,
		      int *errs, int nerrs)
{
	uint8_t omega[FTRFS_RS_PARITY];
	uint8_t err_loc_prime[FTRFS_RS_PARITY];
	int i, j;

	memset(omega, 0, sizeof(omega));
	for (i = 0; i < FTRFS_RS_PARITY; i++) {
		for (j = 0; j < err_loc_len && j <= i; j++)
			omega[i] ^= gf_mul(syndromes[i - j], err_loc[j]);
	}

	memset(err_loc_prime, 0, sizeof(err_loc_prime));
	for (i = 1; i < err_loc_len; i += 2)
		err_loc_prime[i - 1] = err_loc[i];

	for (i = 0; i < nerrs; i++) {
		uint8_t xi     = gf_pow(gf_exp[1], errs[i]);
		uint8_t xi_inv = gf_inv(xi);
		uint8_t omega_val = 0;
		uint8_t elp_val   = 0;

		for (j = FTRFS_RS_PARITY - 1; j >= 0; j--)
			omega_val = gf_mul(omega_val, xi_inv) ^ omega[j];

		for (j = (err_loc_len - 1) & ~1; j >= 0; j -= 2)
			elp_val = gf_mul(elp_val, gf_mul(xi_inv, xi_inv))
				  ^ err_loc_prime[j];

		if (elp_val == 0)
			continue;

		msg[errs[i]] ^= gf_mul(gf_mul(xi, omega_val), gf_inv(elp_val));
	}
}

/*
 * ftrfs_rs_decode - decode and correct a RS(255,239) codeword in place.
 * @data:   FTRFS_SUBBLOCK_DATA bytes of data (corrected in place)
 * @parity: FTRFS_RS_PARITY bytes of parity
 *
 * Returns 0 if no errors or errors corrected,
 * -EBADMSG if uncorrectable (> 8 symbol errors).
 */
int ftrfs_rs_decode(uint8_t *data, uint8_t *parity)
{
	uint8_t msg[FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY];
	uint8_t syndromes[FTRFS_RS_PARITY];
	uint8_t err_loc[FTRFS_RS_PARITY + 1];
	int err_loc_len;
	int errs[FTRFS_RS_PARITY / 2];
	int nerrs, nroots;

	if (!rs_initialized)
		init_gf_tables();

	memcpy(msg, data, FTRFS_SUBBLOCK_DATA);
	memcpy(msg + FTRFS_SUBBLOCK_DATA, parity, FTRFS_RS_PARITY);

	/* Step 1: syndromes */
	if (rs_calc_syndromes(msg, FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY,
			      syndromes))
		return 0; /* no errors */

	/* Step 2: Berlekamp-Massey */
	memset(err_loc, 0, sizeof(err_loc));
	nerrs = rs_berlekamp_massey(syndromes, err_loc, &err_loc_len);
	if (nerrs < 0) {
		pr_err_ratelimited("ftrfs: RS block uncorrectable\n");
		return -EBADMSG;
	}

	/* Step 3: Chien search */
	nroots = rs_chien_search(err_loc, err_loc_len,
				 FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY,
				 errs);
	if (nroots != nerrs) {
		pr_err_ratelimited("ftrfs: RS Chien search mismatch\n");
		return -EBADMSG;
	}

	/* Step 4: Forney corrections */
	rs_forney(msg, syndromes, err_loc, err_loc_len, errs, nerrs);

	/* Step 5: verify */
	if (!rs_calc_syndromes(msg, FTRFS_SUBBLOCK_DATA + FTRFS_RS_PARITY,
			       syndromes)) {
		pr_err_ratelimited("ftrfs: RS correction failed verification\n");
		return -EBADMSG;
	}

	memcpy(data, msg, FTRFS_SUBBLOCK_DATA);
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
