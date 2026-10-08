// SPDX-License-Identifier: GPL-2.0-only
/*
 * Management of the kernel-owned NV index.
 *
 * TPM2_KERNEL_NV_INDEX is an 8 byte ordinary NV index with an empty auth
 * value.  Userspace is prevented from modifying it by the /dev/tpm command
 * filter, so while it holds tpm2_kernel_nv_magic the kernel is the only
 * party that can have put it there.  The kernel sets the magic value only
 * while it holds the chip's ops lock and resets the index to zero before
 * releasing it, so userspace never observes the magic value.
 */

#include <linux/slab.h>
#include <linux/unaligned.h>
#include <crypto/sha2.h>
#include "tpm.h"

#define TPM2_KERNEL_NV_ATTRS \
	(TPM2_NV_AUTHWRITE | TPM2_NV_AUTHREAD | TPM2_NV_NO_DA)

const u8 tpm2_kernel_nv_magic[TPM2_KERNEL_NV_SIZE] = "LNXKEYGN";
static const u8 tpm2_kernel_nv_zero[TPM2_KERNEL_NV_SIZE];

/**
 * tpm2_kernel_nv_name() - compute the expected name of the kernel NV index
 * @written:	whether the index has been written (TPMA_NV_WRITTEN is set)
 * @name:	filled with the name: the SHA256 algorithm ID followed by the
 *		hash of the index's TPMS_NV_PUBLIC
 */
void tpm2_kernel_nv_name(bool written, u8 name[TPM2_NULL_NAME_SIZE])
{
	u32 attrs = TPM2_KERNEL_NV_ATTRS | (written ? TPM2_NV_WRITTEN : 0);
	u8 pub[14];

	/* TPMS_NV_PUBLIC */
	put_unaligned_be32(TPM2_KERNEL_NV_INDEX, &pub[0]);
	put_unaligned_be16(TPM_ALG_SHA256, &pub[4]);
	put_unaligned_be32(attrs, &pub[6]);
	/* authPolicy (empty) */
	put_unaligned_be16(0, &pub[10]);
	put_unaligned_be16(TPM2_KERNEL_NV_SIZE, &pub[12]);

	put_unaligned_be16(TPM_ALG_SHA256, name);
	sha256(pub, sizeof(pub), name + 2);
}

/*
 * Read the name of the kernel NV index.  Returns 0 and fills @name, a
 * positive TPM error code if the index does not exist, or -errno.
 */
static int tpm2_kernel_nv_read_name(struct tpm_chip *chip,
				    u8 name[TPM2_NULL_NAME_SIZE])
{
	struct tpm_buf *buf __free(kfree) = NULL;
	off_t offset = TPM_HEADER_SIZE;
	u16 len;
	int rc;

	buf = kzalloc(TPM_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	tpm_buf_init(buf, TPM_BUFSIZE);
	tpm_buf_reset(buf, TPM2_ST_NO_SESSIONS, TPM2_CC_NV_READ_PUBLIC);
	tpm_buf_append_u32(buf, TPM2_KERNEL_NV_INDEX);

	rc = tpm_transmit_cmd(chip, buf, 0, NULL);
	if (rc)
		return rc;

	/* skip nvPublic */
	if (tpm_buf_length(buf) < offset + sizeof(u16))
		return -EIO;
	offset += sizeof(u16) + get_unaligned_be16(&buf->data[offset]);

	/* nvName */
	if (tpm_buf_length(buf) < offset + sizeof(u16))
		return -EIO;
	len = get_unaligned_be16(&buf->data[offset]);
	offset += sizeof(u16);
	if (len != TPM2_NULL_NAME_SIZE || tpm_buf_length(buf) < offset + len)
		return -EIO;

	memcpy(name, &buf->data[offset], len);
	return 0;
}

static int tpm2_kernel_nv_write(struct tpm_chip *chip, const u8 *value)
{
	return tpm2_nv_write(chip, TPM2_KERNEL_NV_INDEX, TPM2_KERNEL_NV_INDEX,
			     0, value, TPM2_KERNEL_NV_SIZE);
}

/*
 * Determine whether the index may hold the magic value, without changing
 * it.  Only a definite answer that it does not is trusted: the index does
 * not exist, has never been written, or holds some other value.
 */
static bool tpm2_kernel_nv_may_hold_magic(struct tpm_chip *chip)
{
	u8 name[TPM2_NULL_NAME_SIZE], value[TPM2_KERNEL_NV_SIZE];
	int rc;

	rc = tpm2_kernel_nv_read_name(chip, name);
	if (rc > 0 && tpm2_rc_value(rc) == TPM2_RC_HANDLE)
		return false;
	if (rc)
		return true;

	rc = tpm2_nv_read(chip, TPM2_KERNEL_NV_INDEX, TPM2_KERNEL_NV_INDEX, 0,
			  value, sizeof(value));
	if (rc == TPM2_RC_NV_UNINITIALIZED)
		return false;
	if (rc)
		return true;

	return !memcmp(value, tpm2_kernel_nv_magic, sizeof(value));
}

static void tpm2_kernel_nv_update_dirty(struct tpm_chip *chip, bool dirty)
{
	if (dirty)
		chip->flags |= TPM_CHIP_FLAG_KERNEL_NV_DIRTY;
	else
		chip->flags &= ~TPM_CHIP_FLAG_KERNEL_NV_DIRTY;
}

static int tpm2_kernel_nv_recreate(struct tpm_chip *chip)
{
	int rc;

	rc = tpm2_nv_undefine(chip, TPM2_KERNEL_NV_INDEX);
	if (rc)
		return rc;

	rc = tpm2_nv_define(chip, TPM2_KERNEL_NV_INDEX, TPM2_KERNEL_NV_ATTRS,
			    TPM2_KERNEL_NV_SIZE);
	if (rc)
		return rc;

	return tpm2_kernel_nv_write(chip, tpm2_kernel_nv_zero);
}

/*
 * Reset the index to zero unless it already is, to avoid an NV write on
 * every boot.
 */
static int tpm2_kernel_nv_ensure_zero(struct tpm_chip *chip)
{
	u8 value[TPM2_KERNEL_NV_SIZE];
	int rc;

	rc = tpm2_nv_read(chip, TPM2_KERNEL_NV_INDEX, TPM2_KERNEL_NV_INDEX, 0,
			  value, sizeof(value));
	if (!rc && !memcmp(value, tpm2_kernel_nv_zero, sizeof(value)))
		return 0;

	return tpm2_kernel_nv_clear(chip);
}

static int __tpm2_kernel_nv_provision(struct tpm_chip *chip)
{
	u8 expected[TPM2_NULL_NAME_SIZE], name[TPM2_NULL_NAME_SIZE];
	int rc;

	rc = tpm2_kernel_nv_read_name(chip, name);
	if (rc < 0)
		return rc;

	if (rc > 0) {
		/* the index does not exist */
		rc = tpm2_nv_define(chip, TPM2_KERNEL_NV_INDEX,
				    TPM2_KERNEL_NV_ATTRS, TPM2_KERNEL_NV_SIZE);
		if (rc)
			return rc;

		return tpm2_kernel_nv_clear(chip);
	}

	tpm2_kernel_nv_name(true, expected);
	if (memcmp(name, expected, sizeof(name))) {
		tpm2_kernel_nv_name(false, expected);
		if (memcmp(name, expected, sizeof(name))) {
			dev_warn(&chip->dev,
				 "kernel NV index has unexpected attributes, redefining\n");
			rc = tpm2_kernel_nv_recreate(chip);
			if (rc)
				return rc;

			return tpm2_kernel_nv_clear(chip);
		}
	}

	rc = tpm2_kernel_nv_ensure_zero(chip);
	if (rc > 0) {
		/* auth value is not empty: the index was not defined by us */
		dev_warn(&chip->dev,
			 "kernel NV index cannot be written, redefining\n");
		rc = tpm2_kernel_nv_recreate(chip);
		if (rc)
			return rc;

		return tpm2_kernel_nv_clear(chip);
	}

	return rc;
}

/**
 * tpm2_kernel_nv_provision() - ensure the kernel NV index is usable
 * @chip:	the TPM chip
 *
 * Makes sure the kernel NV index exists with the expected public area and
 * an empty auth value, and that it holds zero.  An index with an unexpected
 * public area, or one that cannot be written with an empty auth value, is
 * undefined and defined again.  The owner hierarchy must have an empty auth
 * value.  If this fails and the index may still hold the magic value, the
 * chip is marked so that userspace commands are refused.  The caller must
 * hold the chip's ops lock.
 *
 * Return: 0 on success, -errno or a TPM error code on failure.
 */
int tpm2_kernel_nv_provision(struct tpm_chip *chip)
{
	int rc = __tpm2_kernel_nv_provision(chip);

	tpm2_kernel_nv_update_dirty(chip,
				    rc && tpm2_kernel_nv_may_hold_magic(chip));
	return rc;
}

/**
 * tpm2_kernel_nv_set_magic() - store the magic value in the kernel NV index
 * @chip:	the TPM chip
 *
 * The caller must hold the chip's ops lock, and must call
 * tpm2_kernel_nv_clear() before releasing it.
 *
 * Return: 0 on success, -errno or a TPM error code on failure.
 */
int tpm2_kernel_nv_set_magic(struct tpm_chip *chip)
{
	/* assume the worst until the index is known to be clear again */
	chip->flags |= TPM_CHIP_FLAG_KERNEL_NV_DIRTY;

	return tpm2_kernel_nv_write(chip, tpm2_kernel_nv_magic);
}

/**
 * tpm2_kernel_nv_clear() - reset the kernel NV index to zero
 * @chip:	the TPM chip
 *
 * If the index cannot be reset, it may still hold the magic value, so the
 * chip is marked so that userspace commands are refused until a reset
 * succeeds.  The caller must hold the chip's ops lock.
 *
 * Return: 0 on success, -errno or a TPM error code on failure.
 */
int tpm2_kernel_nv_clear(struct tpm_chip *chip)
{
	int rc;

	rc = tpm2_kernel_nv_write(chip, tpm2_kernel_nv_zero);
	tpm2_kernel_nv_update_dirty(chip,
				    rc && tpm2_kernel_nv_may_hold_magic(chip));
	return rc;
}
