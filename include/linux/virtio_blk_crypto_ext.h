/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_VIRTIO_BLK_CRYPTO_EXT_H
#define __LINUX_VIRTIO_BLK_CRYPTO_EXT_H

#include <linux/blk-crypto.h>

struct blk_crypto_profile;
struct blk_crypto_key;
struct device;
struct request_queue;

#if IS_ENABLED(CONFIG_VIRTBLK_CRYPTO_VIRTUALIZATION)
struct virtblk_crypto_variant_ops {
	/*
	 * Module providing the function pointers below. virtio_blk_crypto_ext.c
	 * pins it with try_module_get()/module_put() around every call, so that
	 * the module implementing these ops can be safely rmmod'd: the unload
	 * will simply block/fail until no dispatch call is in flight, instead
	 * of racing with one.
	 */
	struct module *owner;
	int (*program_key)(const struct blk_crypto_key *key,
			   unsigned int slot);
	int (*evict_key)(unsigned int slot);
	int (*derive_sw_secret_key)(const u8 *eph_key, size_t eph_key_size,
				    u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE]);
	int (*generate_key)(u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
	int (*prepare_key)(const u8 *lt_key, size_t lt_key_size,
			   u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
	int (*import_key)(const u8 *raw_key, size_t raw_key_size,
			  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
};

/*
 * Probes the platform's inline-encryption capabilities and initializes the
 * shared blk_crypto_profile singleton (once) with the keyslot/DUN limits,
 * supported key types, wrapped key size, and supported crypto modes reported
 * by the device over virtio config space / VIRTIO_BLK_T_GET_CRYPTO_MODES.
 *
 * Safe to call from multiple devices, including concurrently: only the
 * first caller actually initializes the shared profile, every other caller
 * just validates its own capabilities against what was already negotiated.
 */
int virtblk_init_inline_crypto(unsigned int max_slots, unsigned int max_dun_bytes,
				unsigned int key_types,
				const unsigned int crypto_modes_supported[BLK_ENCRYPTION_MODE_MAX],
				struct device *dev);

/*
 * Registers the (already-initialized) shared blk_crypto_profile with the
 * given request queue. Returns false (and leaves the queue without inline
 * crypto) if the queue's block-integrity support conflicts with inline
 * encryption; see blk_crypto_register().
 */
bool virtblk_crypto_register(struct request_queue *q);

void virtblk_set_crypto_ops(struct virtblk_crypto_variant_ops *ops);

#else

static inline int virtblk_init_inline_crypto(unsigned int max_slots,
		unsigned int max_dun_bytes, unsigned int key_types,
		const unsigned int crypto_modes_supported[BLK_ENCRYPTION_MODE_MAX],
		struct device *dev)
{
	return -EOPNOTSUPP;
}

static inline bool virtblk_crypto_register(struct request_queue *q)
{
	return false;
}


#endif

#endif /* __LINUX_VIRTIO_BLK_CRYPTO_EXT_H */
