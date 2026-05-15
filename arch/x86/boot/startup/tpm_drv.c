// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based of the original tpm_tis.c implementation as found in the
 * Linux 2.6 code base.
 *
 * Copyright (C) 2005, 2006 IBM Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 */

#include <crypto/sha2.h>
#include <asm/io.h>

#include <linux/tpm_command.h>
#include <linux/tpm_ptp.h>
#include <linux/tpm_buf.h>

#include "tpm.h"

static u8 tpm_buf_page[PAGE_SIZE];

/*
 * Single threaded environment only running on BSP. Use a single shared
 * page for all TPM extend operations.
 */
static inline struct tpm_buf *tpm_buf_alloc_page(void)
{
	memset(tpm_buf_page, 0, PAGE_SIZE);
	return (struct tpm_buf *)tpm_buf_page;
}

static inline void tpm_buf_free_page(void)
{
	memset(tpm_buf_page, 0, PAGE_SIZE);
}

/* Pull in TPM buffer management support */
#include "../../../../drivers/char/tpm/tpm-buf.c"

static u32 __init tpm_get_alg_size(u16 alg_id)
{
	switch (alg_id) {
	case TPM_ALG_SHA1:
		return TPM_DIGEST_SIZE;
	case TPM_ALG_SHA256:
	case TPM_ALG_SM3_256:
		return SHA256_DIGEST_SIZE;
	case TPM_ALG_SHA384:
		return SHA384_DIGEST_SIZE;
	case TPM_ALG_SHA512:
	default:
		return SHA512_DIGEST_SIZE;
	};
}

static inline u8 tpm_read8(struct tpm_chip *chip, u32 field)
{
	return readb((void *)(chip->baseaddr | field));
}

static inline void tpm_write8(struct tpm_chip *chip, u32 field, u8 val)
{
	writeb(val, (void *)(chip->baseaddr | field));
}

static inline u32 tpm_read32(struct tpm_chip *chip, u32 field)
{
	return readl((void *)(chip->baseaddr | field));
}

static inline void tpm_write32(struct tpm_chip *chip, u32 field, u32 val)
{
	writel(val, (void *)(chip->baseaddr | field));
}

/*
 * In the setup kernel environment, it is far too early to calibrate time.
 * Assume a 5GHz processor (the upper end of the Fam19h range), allowing
 * reasonable timeouts on slower systems.
 */
static unsigned long ticks_per_ms = (5UL * 1000 * 1000 /* CPU in KHz */);

static inline ktime_t tpm_now_ms(void)
{
	return rdtsc()/ticks_per_ms;
}

static inline void tpm_mdelay(unsigned int msecs)
{
	unsigned long ticks = msecs * ticks_per_ms;
	unsigned long s, e;

	s = rdtsc();
	do {
		cpu_relax();
		e = rdtsc();
	} while ((e - s) < ticks);
}

static inline u8 __tis_status(struct tpm_chip *chip)
{
	return tpm_read8(chip, TPM_STS(chip->locality));
}

static inline void __tis_cancel(struct tpm_chip *chip)
{
	/* This causes the current command to be aborted */
	tpm_write8(chip, TPM_STS(chip->locality), TPM_STS_COMMAND_READY);
}

static int __init __tis_get_burstcount(struct tpm_chip *chip)
{
	ktime_t stop;
	int burstcnt;

	stop = tpm_now_ms() + chip->timeout_d;
	do {
		burstcnt = tpm_read8(chip, (TPM_STS(chip->locality) + 1));
		burstcnt += tpm_read8(chip, TPM_STS(chip->locality) + 2) << 8;

		if (burstcnt)
			return burstcnt;

		tpm_mdelay(TPM_TIMEOUT);
	} while (tpm_now_ms() < stop);

	return -EBUSY;
}

static int __init __tis_wait_for_stat(struct tpm_chip *chip, u8 mask, ktime_t timeout)
{
	ktime_t stop;
	u8 status;

	if ((__tis_status(chip) & mask) == mask)
		return 0;

	stop = tpm_now_ms() + timeout;
	do {
		tpm_mdelay(TPM_TIMEOUT);

		status = __tis_status(chip);
		if ((status & mask) == mask)
			return 0;
	} while (tpm_now_ms() < stop);

	return -ETIME;
}

static int __init __tis_recv_data(struct tpm_chip *chip, u8 *buf, int count)
{
	int size = 0;
	int burstcnt;

	while (size < count &&
	       __tis_wait_for_stat(chip,
				   TPM_STS_DATA_AVAIL | TPM_STS_VALID,
				   chip->timeout_c) == 0) {
		burstcnt = __tis_get_burstcount(chip);

		for ( ; burstcnt > 0 && size < count; --burstcnt)
			buf[size++] = tpm_read8(chip, TPM_DATA_FIFO(chip->locality));
	}

	return size;
}

/**
 * tpm_tis_check_locality - Check if the given locality is the active one
 * @chip:	The TPM chip instance
 * @loc:	The locality to check
 *
 * Return: true - locality active, false - not active
 */
bool __init tpm_tis_check_locality(struct tpm_chip *chip, int loc)
{
	u8 res = tpm_read8(chip, TPM_ACCESS(loc));

	if ((res & (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) ==
		   (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) {
		chip->locality = loc;
		return true;
	}

	return false;
}

/**
 * tpm_tis_release_locality - Release the active locality
 * @chip:	The TPM chip instance
 */
void __init tpm_tis_release_locality(struct tpm_chip *chip)
{
	u8 res = tpm_read8(chip, TPM_ACCESS(chip->locality));

	if ((res & (TPM_ACCESS_REQUEST_PENDING | TPM_ACCESS_VALID)) ==
		   (TPM_ACCESS_REQUEST_PENDING | TPM_ACCESS_VALID))
		tpm_write8(chip, TPM_ACCESS(chip->locality), TPM_ACCESS_RELINQUISH_LOCALITY);

	chip->locality = 0;
}

/**
 * tpm_tis_request_locality - Request to make the given locality the active one
 * @chip:	The TPM chip instance
 * @loc:	The locality to make active/set as current
 *
 * Return:
 *  >= 0 - Success, new active locality returned or locality already active
 *  < 0  - Error occurred
 */
int __init tpm_tis_request_locality(struct tpm_chip *chip, int loc)
{
	ktime_t stop;

	if (tpm_tis_check_locality(chip, loc))
		return loc;

	/* Set the new locality */
	tpm_write8(chip, TPM_ACCESS(loc), TPM_ACCESS_REQUEST_USE);

	stop = tpm_now_ms() + chip->timeout_b;
	do {
		if (tpm_tis_check_locality(chip, loc))
			return loc;

		tpm_mdelay(TPM_TIMEOUT);
	} while (tpm_now_ms() < stop);

	return -1;
}

/**
 * tpm_tis_disable_interrupts - Disable interrupts for the TPM, use polling mode only
 * @chip:	The TPM chip instance
 */
void __init tpm_tis_disable_interrupts(struct tpm_chip *chip)
{
	u32 intmask;

	intmask = tpm_read32(chip, TPM_INT_ENABLE(chip->locality));
	/* Disable everything to make sure it is in a consistent state */
	intmask &= ~(TPM_GLOBAL_INT_ENABLE | TPM_INTF_CMD_READY_INT |
		     TPM_INTF_LOCALITY_CHANGE_INT | TPM_INTF_STS_VALID_INT |
		     TPM_INTF_DATA_AVAIL_INT);
	tpm_write32(chip, TPM_INT_ENABLE(chip->locality), intmask);
}

/**
 * tpm_tis_recv - Receive response data from TPM via TIS FIFO
 * @chip:	The TPM chip instance
 * @buf:	The response buffer
 * @count:	Length of the response buffer
 *
 * Return:
 *  = 0 - Success, no response data
 *  > 0 - Success, value is the response data length
 *  < 0 - Error occurred
 */
static int __init tpm_tis_recv(struct tpm_chip *chip, u8 *buf, int count)
{
	int expected, status, size = 0, rc = -EIO;

	if (count < TPM_HEADER_SIZE)
		goto out;

	/* Read first 10 bytes, including tag, paramsize, and result */
	size = __tis_recv_data(chip, buf, TPM_HEADER_SIZE);
	if (size < TPM_HEADER_SIZE)
		goto out;

	expected = be32_to_cpu(*((u32 *)(buf + 2)));
	if (expected > count)
		goto out;

	size += __tis_recv_data(chip, &buf[TPM_HEADER_SIZE], expected - TPM_HEADER_SIZE);
	if (size < expected) {
		rc = -ETIME;
		goto out;
	}

	__tis_wait_for_stat(chip, TPM_STS_VALID, chip->timeout_c);

	status = __tis_status(chip);
	if (status & TPM_STS_DATA_AVAIL) {
		rc = -EIO;
		goto out;
	}

	/* Done with receive, move to Command Ready state */
	__tis_cancel(chip);

	return size;
out:
	__tis_cancel(chip);
	tpm_tis_release_locality(chip);
	return rc;
}

/**
 * tpm_tis_send - Send command to TPM via TIS FIFO
 * @chip:	The TPM chip instance
 * @buf:	The command buffer
 * @len:	Length of the command buffer to send
 *
 * Return:
 *  = len - Success, all data sent
 *  < 0	  - Error occurred
 */
static int __init tpm_tis_send(struct tpm_chip *chip, u8 *buf, int len)
{
	int status, burstcnt = 0;
	int count = 0;
	int rc = 0;

	status = __tis_status(chip);
	if ((status & TPM_STS_COMMAND_READY) == 0) {
		__tis_cancel(chip);
		if (__tis_wait_for_stat(chip, TPM_STS_COMMAND_READY, chip->timeout_b) < 0) {
			rc = -ETIME;
			goto out_err;
		}
	}

	while (count < len - 1) {
		burstcnt = __tis_get_burstcount(chip);
		for ( ; burstcnt > 0 && count < len - 1; --burstcnt)
			tpm_write8(chip, TPM_DATA_FIFO(chip->locality), buf[count++]);

		__tis_wait_for_stat(chip, TPM_STS_VALID, chip->timeout_c);
		status = __tis_status(chip);
		if ((status & TPM_STS_DATA_EXPECT) == 0) {
			rc = -EIO;
			goto out_err;
		}
	}

	/* Write last byte */
	tpm_write8(chip, TPM_DATA_FIFO(chip->locality), buf[count]);
	__tis_wait_for_stat(chip, TPM_STS_VALID, chip->timeout_c);
	status = __tis_status(chip);
	if ((status & TPM_STS_DATA_EXPECT) != 0) {
		rc = -EIO;
		goto out_err;
	}

	/* Go and do it */
	tpm_write8(chip, TPM_STS(chip->locality), TPM_STS_GO);

	return len;

out_err:
	__tis_cancel(chip);
	tpm_tis_release_locality(chip);
	return rc;
}

/**
 * tpm_tis_transmit - Transmit a TPM FIFO command
 * @chip:	The TPM chip instance
 * @buf:	The request and response buffer object
 * @bufsize:	Entire size available in buffer
 *
 * Return:
 *  = 0 - Success, no returned data
 *  > 0 - Success, value is the return data length
 *  < 0 - Error occurred
 */
static int __init tpm_tis_transmit(struct tpm_chip *chip, u8 *buf, u32 bufsize)
{
	ktime_t stop;
	u32 count;
	u8 status;
	int rc;

	count = be32_to_cpu(*((u32 *) (buf + 2)));
	if (count == 0)
		return -ENODATA;

	if (count > bufsize)
		return -E2BIG;

	rc = tpm_tis_send(chip, buf, count);
	if (rc < 0)
		goto out;

	stop = tpm_now_ms() + TIS_DURATION;
	do {
		status = __tis_status(chip);
		if ((status & (TPM_STS_DATA_AVAIL | TPM_STS_VALID)) ==
			      (TPM_STS_DATA_AVAIL | TPM_STS_VALID))
			goto out_recv;

		if (status == TPM_STS_COMMAND_READY) {
			rc = -ECANCELED;
			goto out;
		}

		tpm_mdelay(TPM_TIMEOUT);
		rmb();
	} while (tpm_now_ms() < stop);

	/* Cancel the command */
	__tis_cancel(chip);
	rc = -ETIME;
	goto out;

out_recv:
	rc = tpm_tis_recv(chip, buf, bufsize);
	if (rc >= 0) {
		if (rc > 0 && rc < TPM_HEADER_SIZE)
			return -EFAULT;
		return rc;
	}
	/* Else return was an error, nothing to receive */

out:
	return rc;
}

/**
 * tpm_find_interface_and_family - interface FIFO/CRB, family 2.0 or 1.2
 * @chip:	The TPM chip instance
 *
 * Return: TPM family ID enum
 */
static enum tpm_family __init tpm_find_interface_and_family(struct tpm_chip *chip)
{
	struct tpm_intf_capability intf_cap;
	struct tpm_interface_id intf_id;

	/* Sort out whether it is 1.x */
	intf_cap.val = tpm_read32(chip, TPM_INTF_CAPS(0));
	if ((intf_cap.interface_version == TPM_TIS_INTF_12) ||
	    (intf_cap.interface_version == TPM_TIS_INTF_13))
		return TPM_FAMILY_12; /* Always TIS */

	/* Assume that it is 2.0 but check if the interface is CRB */
	intf_id.val = tpm_read32(chip, TPM_INTF_ID(0));
	if (intf_id.interface_type == TPM_CRB_INTF_ACTIVE)
		return TPM_FAMILY_INVALID;

	/* Else TPM 2.0 with TIS interface */
	return TPM_FAMILY_20;
}

/**
 * tpm1_pcr_extend - send a TPM1 extend command to the device
 * @chip:	a TPM chip to use
 * @pcr_idx:	the PCR index to extend for the current locality
 * @hash:	the SHA1 hash digest to extend
 *
 * Return:
 * * 0		- OK
 * * -errno	- A system error
 * * TPM_RC	- A TPM error
 */
int __init tpm1_pcr_extend(struct tpm_chip *chip, u32 pcr_idx, const u8 *hash)
{
	struct tpm_buf *buf = tpm_buf_alloc_page();
	int rc = 0;

	tpm_buf_init(buf, TPM_BUFSIZE);
	tpm_buf_reset(buf, TPM_TAG_RQU_COMMAND, TPM_ORD_PCR_EXTEND);

	tpm_buf_append_u32(buf, pcr_idx);
	tpm_buf_append(buf, hash, TPM_DIGEST_SIZE);

	rc = tpm_tis_transmit(chip, buf->data, PAGE_SIZE);

	/* Ignoring output */
	if (rc > 0)
		rc = 0;

	tpm_buf_free_page();

	return rc;
}

/**
 * tpm2_pcr_extend() - send a TPM2 extend command to the device
 *
 * @chip:		TPM chip to use.
 * @pcr_idx:		index of the PCR.
 * @digests:		list of PCR banks and corresponding digest values to extend.
 * @digest_count:	count of digests to extend
 *
 * Return:
 * * 0		- OK
 * * -errno	- A system error
 * * TPM_RC	- A TPM error
 */
int __init tpm2_pcr_extend(struct tpm_chip *chip, u32 pcr_idx,
			   struct tpm_digest *digests, u32 digest_count)
{
	struct tpm_buf *buf = tpm_buf_alloc_page();
	int rc = 0, i;

	tpm_buf_init(buf, TPM_BUFSIZE);
	tpm_buf_reset(buf, TPM2_ST_SESSIONS, TPM2_CC_PCR_EXTEND);

	tpm_buf_append_u32(buf, pcr_idx);

	/* Setup a NULL auth session for the command */
	tpm_buf_append_u32(buf, 9);
	/* auth handle */
	tpm_buf_append_u32(buf, TPM2_RS_PW);
	/* nonce */
	tpm_buf_append_u16(buf, 0);
	/* attributes */
	tpm_buf_append_u8(buf, 0);
	/* passphrase */
	tpm_buf_append_u16(buf, 0);

	tpm_buf_append_u32(buf, digest_count);

	for (i = 0; i < digest_count; i++) {
		tpm_buf_append_u16(buf, digests[i].alg_id);
		tpm_buf_append(buf, (const unsigned char *)&digests[i].digest,
			       tpm_get_alg_size(digests[i].alg_id));
	}

	rc = tpm_tis_transmit(chip, buf->data, PAGE_SIZE);

	/* Ignoring output */
	if (rc > 0)
		rc = 0;

	tpm_buf_free_page();

	return rc;
}

int __init early_tpm_init(struct tpm_chip *chip, u64 baseaddr)
{
	u32 didvid;

	memset(chip, 0, sizeof(*chip));
	chip->baseaddr = baseaddr;

	chip->family = tpm_find_interface_and_family(chip);
	if (chip->family == TPM_FAMILY_INVALID)
		return TPM_ERR_INVALID_FAMILY;

	/* Set default timeouts */
	chip->timeout_a = TIS_SHORT_TIMEOUT;
	chip->timeout_b = TIS_LONG_TIMEOUT;
	chip->timeout_c = TIS_SHORT_TIMEOUT;
	chip->timeout_d = TIS_SHORT_TIMEOUT;

	/* Get the vendor and device ids */
	didvid = tpm_read32(chip, TPM_DID_VID(0));
	chip->did = didvid >> 16;
	chip->vid = didvid & 0xFFFF;

	return TPM_SUCCESS;
}

int __init early_tpm_fini(struct tpm_chip *chip)
{
	tpm_tis_release_locality(chip);
	memset(chip, 0, sizeof(*chip));

	return TPM_SUCCESS;
}
