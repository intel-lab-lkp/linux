// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/container_of.h>
#include <linux/blk-crypto.h>
#include <linux/blk-crypto-profile.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_blk_crypto_ext.h>

struct virtblk_crypto_profile {
	struct blk_crypto_profile profile;
	struct virtblk_crypto_variant_ops *ops;
};

static struct virtblk_crypto_profile g_vdcp;
static bool g_crypto_profile_initialized;
static struct device *virtblk_profile_owner;
static unsigned int g_max_slots;
static unsigned int g_max_dun_bytes;
static unsigned int g_key_types;
static DEFINE_MUTEX(virtblk_crypto_init_lock);
static DEFINE_MUTEX(virtblk_crypto_ops_lock);

bool virtblk_crypto_register(struct request_queue *q)
{
	return blk_crypto_register(&g_vdcp.profile, q);
}
EXPORT_SYMBOL_GPL(virtblk_crypto_register);

/*
 * Returns the variant ops registered for @profile's virtblk_crypto_profile
 * with a module reference held on ops->owner, or NULL if none are
 * registered. Pairs with virtblk_crypto_ops_put().
 *
 * Holding a module reference for the duration of each dispatch call (rather
 * than just dereferencing the raw pointer) is what makes it safe for the
 * module that implements these ops (e.g. drivers/soc/qcom/crypto_virt.c) to
 * be rmmod'd: module removal will fail/block until every in-flight dispatch
 * call has released its reference, instead of racing with a concurrent
 * virtblk_set_crypto_ops(NULL) and the ops table disappearing mid-call.
 */
static struct virtblk_crypto_variant_ops *
virtblk_crypto_ops_get(struct blk_crypto_profile *profile)
{
	struct virtblk_crypto_profile *vdcp =
		container_of(profile, struct virtblk_crypto_profile, profile);
	struct virtblk_crypto_variant_ops *ops;

	mutex_lock(&virtblk_crypto_ops_lock);
	ops = vdcp->ops;
	if (ops && !try_module_get(ops->owner))
		ops = NULL;
	mutex_unlock(&virtblk_crypto_ops_lock);

	return ops;
}

static void virtblk_crypto_ops_put(struct virtblk_crypto_variant_ops *ops)
{
	module_put(ops->owner);
}

static int virtblk_crypto_keyslot_program(struct blk_crypto_profile *profile,
					   const struct blk_crypto_key *key,
					   unsigned int slot)
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->program_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->program_key(key, slot);
	virtblk_crypto_ops_put(ops);
	if (ret)
		pr_err("program hardware wrapped key failed: slot=%u ret=%d\n", slot, ret);

	return ret;
}

static int virtblk_crypto_keyslot_evict(struct blk_crypto_profile *profile,
					 const struct blk_crypto_key *key,
					 unsigned int slot)
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->evict_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->evict_key(slot);
	virtblk_crypto_ops_put(ops);
	if (ret)
		pr_err("evict keyslot %u failed: %d\n", slot, ret);

	return ret;
}

static int virtblk_crypto_derive_sw_secret(struct blk_crypto_profile *profile,
					    const u8 *eph_key,
					    size_t eph_key_size,
					    u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE])
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->derive_sw_secret_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->derive_sw_secret_key(eph_key, eph_key_size, sw_secret);
	virtblk_crypto_ops_put(ops);
	if (ret)
		pr_err("derive software secret failed: %d\n", ret);

	return ret;
}

static int virtblk_crypto_generate_key(struct blk_crypto_profile *profile,
					u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->generate_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->generate_key(lt_key);
	virtblk_crypto_ops_put(ops);
	if (ret < 0)
		pr_err("generate hardware wrapped key failed: %d\n", ret);

	return ret;
}

static int virtblk_crypto_prepare_key(struct blk_crypto_profile *profile,
				       const u8 *lt_key, size_t lt_key_size,
				       u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->prepare_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->prepare_key(lt_key, lt_key_size, eph_key);
	virtblk_crypto_ops_put(ops);
	if (ret < 0)
		pr_err("prepare hardware wrapped key failed: %d\n", ret);

	return ret;
}

static int virtblk_crypto_import_key(struct blk_crypto_profile *profile,
				      const u8 *raw_key, size_t raw_key_size,
				      u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	struct virtblk_crypto_variant_ops *ops = virtblk_crypto_ops_get(profile);
	int ret;

	if (!ops || !ops->import_key) {
		if (ops)
			virtblk_crypto_ops_put(ops);
		return -EOPNOTSUPP;
	}

	ret = ops->import_key(raw_key, raw_key_size, lt_key);
	virtblk_crypto_ops_put(ops);
	if (ret < 0)
		pr_err("import hardware wrapped key failed: %d\n", ret);

	return ret;
}

static const struct blk_crypto_ll_ops virtblk_crypto_ops = {
	.keyslot_program	= virtblk_crypto_keyslot_program,
	.keyslot_evict		= virtblk_crypto_keyslot_evict,
	.derive_sw_secret	= virtblk_crypto_derive_sw_secret,
	.generate_key		= virtblk_crypto_generate_key,
	.prepare_key		= virtblk_crypto_prepare_key,
	.import_key		= virtblk_crypto_import_key,
};

int virtblk_init_inline_crypto(unsigned int max_slots, unsigned int max_dun_bytes,
			       unsigned int key_types,
			       const unsigned int crypto_modes_supported[BLK_ENCRYPTION_MODE_MAX],
			       struct device *dev)
{
	struct blk_crypto_profile *profile = &g_vdcp.profile;
	unsigned int key_type_supported = 0;
	int err = 0;

	dev_info(dev, "probing inline crypto capabilities\n");

	mutex_lock(&virtblk_crypto_init_lock);

	/*
	 * profile is a single, process-wide blk_crypto_profile shared by every
	 * VIRTIO_BLK_F_INLINE_ENCRYPTION device. Only the first device to get
	 * here actually initializes it; any other device just reuses it as-is
	 * if its negotiated capabilities match. A mismatch means this device's
	 * capabilities don't actually correspond to what the shared profile
	 * was set up for (wrong keyslot count, DUN size, or key types), which
	 * is a correctness/security concern, not just a cosmetic one -- fail
	 * instead of silently registering a profile that doesn't match what
	 * this device supports.
	 */
	if (g_crypto_profile_initialized) {
		if (max_slots != g_max_slots || max_dun_bytes != g_max_dun_bytes ||
		    key_types != g_key_types) {
			dev_warn(dev,
				 "inline crypto profile already initialized by %s (max_slots=%u max_dun_bytes=%u key_types=0x%x); "
				 "this device reports max_slots=%u max_dun_bytes=%u key_types=0x%x -- sharing one "
				 "blk_crypto_profile across multiple VIRTIO_BLK_F_INLINE_ENCRYPTION devices with differing "
				 "capabilities is not supported, refusing to enable inline crypto for this device\n",
				 dev_name(virtblk_profile_owner), g_max_slots, g_max_dun_bytes, g_key_types,
				 max_slots, max_dun_bytes, key_types);
			err = -EINVAL;
		}
		goto out_unlock;
	}

	if (key_types & VIRTIO_BLK_CRYPTO_KEY_TYPE_RAW)
		key_type_supported |= BLK_CRYPTO_KEY_TYPE_RAW;
	if (key_types & VIRTIO_BLK_CRYPTO_KEY_TYPE_HW_WRAPPED)
		key_type_supported |= BLK_CRYPTO_KEY_TYPE_HW_WRAPPED;

	err = blk_crypto_profile_init(profile, max_slots);
	if (err) {
		dev_err(dev, "crypto profile initialization failed: %d\n", err);
		goto out_unlock;
	}

	profile->ll_ops = virtblk_crypto_ops;
	profile->max_dun_bytes_supported = max_dun_bytes;
	profile->key_types_supported = key_type_supported;
	profile->dev = dev;
	memcpy(profile->modes_supported, crypto_modes_supported,
	       BLK_ENCRYPTION_MODE_MAX * sizeof(unsigned int));

	virtblk_profile_owner = dev;
	g_max_slots = max_slots;
	g_max_dun_bytes = max_dun_bytes;
	g_key_types = key_types;
	g_crypto_profile_initialized = true;

	dev_info(dev, "inline crypto profile initialized\n");

out_unlock:
	mutex_unlock(&virtblk_crypto_init_lock);
	return err;
}
EXPORT_SYMBOL_GPL(virtblk_init_inline_crypto);

void virtblk_set_crypto_ops(struct virtblk_crypto_variant_ops *ops)
{
	if (!g_crypto_profile_initialized)
		pr_warn("virtio blk crypto profile hasn't been initialized\n");

	mutex_lock(&virtblk_crypto_ops_lock);
	g_vdcp.ops = ops;
	mutex_unlock(&virtblk_crypto_ops_lock);
}
EXPORT_SYMBOL_GPL(virtblk_set_crypto_ops);

MODULE_DESCRIPTION("Virtio block inline crypto extension");
MODULE_LICENSE("GPL");
