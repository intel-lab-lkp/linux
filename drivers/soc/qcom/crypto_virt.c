// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/types.h>
#include <linux/blk-crypto.h>
#include <linux/virtio_blk_crypto_ext.h>
#include <linux/firmware/qcom/qcom_scm.h>

static int crypto_virt_program_key(const struct blk_crypto_key *key,
				   unsigned int slot)
{
	u32 dus_512_units;
	int ret;

	if (!key || !key->size) {
		pr_err("%s: invalid key\n", __func__);
		return -EINVAL;
	}

	/* Only AES-256-XTS has been tested so far. */
	if (key->crypto_cfg.crypto_mode !=
	    BLK_ENCRYPTION_MODE_AES_256_XTS) {
		pr_err_ratelimited("Unsupported crypto mode: %d\n",
				    key->crypto_cfg.crypto_mode);
		return -EINVAL;
	}

	/* qcom_scm_ice_set_key()'s data_unit_size is expressed in 512-byte units */
	dus_512_units = key->crypto_cfg.data_unit_size / 512;

	ret = qcom_scm_ice_set_key(slot, key->bytes, key->size,
		QCOM_SCM_ICE_CIPHER_AES_256_XTS, dus_512_units);
	if (ret)
		pr_err("%s: slot=%u ret=%d\n", __func__, slot, ret);

	return ret;
}

static int crypto_virt_invalidate_key(unsigned int slot)
{
	int ret;

	ret = qcom_scm_ice_invalidate_key(slot);
	if (ret)
		pr_err("%s: slot=%u ret=%d\n", __func__, slot, ret);

	return ret;
}

static int crypto_virt_derive_sw_secret_key(const u8 *eph_key, size_t eph_key_size,
					    u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE])
{
	int ret;

	ret = qcom_scm_derive_sw_secret(eph_key, eph_key_size,
					sw_secret, BLK_CRYPTO_SW_SECRET_SIZE);
	if (ret == -EIO || ret == -EINVAL)
		ret = -EBADMSG; /* probably invalid key */

	if (ret)
		pr_err("%s: ret=%d\n", __func__, ret);

	return ret;
}

static struct virtblk_crypto_variant_ops virtblk_crypto_qcom_vops = {
	.owner = THIS_MODULE,
	.program_key = crypto_virt_program_key,
	.evict_key = crypto_virt_invalidate_key,
	.derive_sw_secret_key = crypto_virt_derive_sw_secret_key,
};

static int __init crypto_virt_init(void)
{
	virtblk_set_crypto_ops(&virtblk_crypto_qcom_vops);
	return 0;
}
module_init(crypto_virt_init);

#if IS_MODULE(CONFIG_QCOM_CRYPTO_VIRT)
static void __exit crypto_virt_exit(void)
{
	virtblk_set_crypto_ops(NULL);
}
module_exit(crypto_virt_exit);
#endif

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Crypto Virt Driver");
MODULE_LICENSE("GPL");
