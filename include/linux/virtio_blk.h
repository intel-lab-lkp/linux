/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VIRTIO_BLK_H
#define _LINUX_VIRTIO_BLK_H

#include <linux/blk-crypto.h>
#include <uapi/linux/virtio_blk.h>

#if IS_ENABLED(CONFIG_VIRTIO_BLK_INLINE_ENCRYPTION)
/**
 * virtio_mode_to_blk() - Convert a virtio_blk crypto mode number to a block mode
 * @vmode: The virtio_blk crypto mode number (VIRTIO_BLK_CRYPTO_MODE_*).
 *
 * Return: The corresponding &enum blk_crypto_mode_num, or
 *         %BLK_ENCRYPTION_MODE_INVALID if @vmode is out of range.
 */
static inline enum blk_crypto_mode_num virtio_mode_to_blk(unsigned int vmode)
{
	/* Indexed by virtio_blk crypto mode number; unlisted entries are 0 (INVALID). */
	static const enum blk_crypto_mode_num modes[__VIRTIO_BLK_CRYPTO_MODE_MAX] = {
		[VIRTIO_BLK_CRYPTO_MODE_AES_256_XTS] = BLK_ENCRYPTION_MODE_AES_256_XTS,
	};

	if (vmode >= __VIRTIO_BLK_CRYPTO_MODE_MAX)
		return BLK_ENCRYPTION_MODE_INVALID;
	return modes[vmode];
}

/**
 * blk_mode_to_virtio() - Convert a block crypto mode to a virtio_blk crypto mode number
 * @bmode: The kernel &enum blk_crypto_mode_num.
 *
 * Return: The corresponding virtio_blk crypto mode number, or
 *         %VIRTIO_BLK_CRYPTO_MODE_INVALID if @bmode is out of range.
 */
static inline unsigned int blk_mode_to_virtio(enum blk_crypto_mode_num bmode)
{
	/* Indexed by blk_crypto_mode_num; unlisted entries are 0 (INVALID). */
	static const unsigned int modes[BLK_ENCRYPTION_MODE_MAX] = {
		[BLK_ENCRYPTION_MODE_AES_256_XTS] = VIRTIO_BLK_CRYPTO_MODE_AES_256_XTS,
	};

	if (bmode >= BLK_ENCRYPTION_MODE_MAX)
		return VIRTIO_BLK_CRYPTO_MODE_INVALID;
	return modes[bmode];
}

/**
 * virtio_key_type_to_blk() - Convert a virtio_blk crypto key type to a block key type
 * @vtype: The virtio_blk crypto key type (VIRTIO_BLK_CRYPTO_KEY_TYPE_*).
 *
 * Return: The corresponding &enum blk_crypto_key_type, or 0 if @vtype does
 *         not name a single supported key type.
 */
static inline enum blk_crypto_key_type virtio_key_type_to_blk(unsigned int vtype)
{
	switch (vtype) {
	case VIRTIO_BLK_CRYPTO_KEY_TYPE_RAW:
		return BLK_CRYPTO_KEY_TYPE_RAW;
	case VIRTIO_BLK_CRYPTO_KEY_TYPE_HW_WRAPPED:
		return BLK_CRYPTO_KEY_TYPE_HW_WRAPPED;
	default:
		return 0;
	}
}

/**
 * blk_key_type_to_virtio() - Convert a block key type to a virtio_blk crypto key type
 * @btype: The kernel &enum blk_crypto_key_type.
 *
 * Return: The corresponding virtio_blk crypto key type
 *         (VIRTIO_BLK_CRYPTO_KEY_TYPE_*), or 0 if @btype does not name a
 *         single supported key type.
 */
static inline unsigned int blk_key_type_to_virtio(enum blk_crypto_key_type btype)
{
	switch (btype) {
	case BLK_CRYPTO_KEY_TYPE_RAW:
		return VIRTIO_BLK_CRYPTO_KEY_TYPE_RAW;
	case BLK_CRYPTO_KEY_TYPE_HW_WRAPPED:
		return VIRTIO_BLK_CRYPTO_KEY_TYPE_HW_WRAPPED;
	default:
		return 0;
	}
}
#endif /* CONFIG_VIRTIO_BLK_INLINE_ENCRYPTION */

#endif /* _LINUX_VIRTIO_BLK_H */
