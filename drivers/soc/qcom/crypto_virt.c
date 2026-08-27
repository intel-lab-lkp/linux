// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/blk-crypto.h>
#include <linux/virtio_blk_crypto_ext.h>
#include <linux/firmware/qcom/qcom_scm.h>

static unsigned int g_wrapped_key_size;

static int crypto_virt_program_key(const struct blk_crypto_key *key,
				   unsigned int slot)
{
	u32 dus_512_units;
	int ret;

	if (!key || !key->size) {
		pr_err("%s: invalid key\n", __func__);
		return -EINVAL;
	}

	/* Only AES-256-XTS is supported so far. */
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

static int crypto_virt_generate_key(u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	if (!g_wrapped_key_size) {
		pr_err("%s: no expected wrapped key size\n", __func__);
		return -EINVAL;
	}

	ret = qcom_scm_generate_ice_key(lt_key, g_wrapped_key_size);
	if (ret) {
		pr_err("%s: generate hardware wrapped key failed: %d\n", __func__, ret);
		return ret;
	}

	return g_wrapped_key_size;
}

static int crypto_virt_prepare_key(const u8 *lt_key, size_t lt_key_size,
				   u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	if (!g_wrapped_key_size) {
		pr_err("%s: no expected wrapped key size\n", __func__);
		return -EINVAL;
	}

	ret = qcom_scm_prepare_ice_key(lt_key, lt_key_size,
					eph_key, g_wrapped_key_size);
	if (ret == -EIO || ret == -EINVAL)
		ret = -EBADMSG; /* probably invalid key */

	if (ret) {
		pr_err("%s: prepare hardware wrapped key failed: %d\n", __func__, ret);
		return ret;
	}

	return g_wrapped_key_size;
}

static int crypto_virt_import_key(const u8 *raw_key, size_t raw_key_size,
				  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	if (!g_wrapped_key_size) {
		pr_err("%s: no expected wrapped key size\n", __func__);
		return -EINVAL;
	}

	ret = qcom_scm_import_ice_key(raw_key, raw_key_size,
				       lt_key, g_wrapped_key_size);
	if (ret) {
		pr_err("%s: import hardware wrapped key failed: %d\n", __func__, ret);
		return ret;
	}

	return g_wrapped_key_size;
}

static struct virtblk_crypto_variant_ops virtblk_crypto_qcom_vops = {
	.owner = THIS_MODULE,
	.program_key = crypto_virt_program_key,
	.evict_key = crypto_virt_invalidate_key,
	.derive_sw_secret_key = crypto_virt_derive_sw_secret_key,
	.generate_key = crypto_virt_generate_key,
	.prepare_key = crypto_virt_prepare_key,
	.import_key = crypto_virt_import_key,
};

static int crypto_virt_probe(struct platform_device *pdev)
{
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,wrapped-key-size",
				    &g_wrapped_key_size);
	if (ret)
		dev_warn(&pdev->dev, "qcom,wrapped-key-size not found\n");

	if (!g_wrapped_key_size ||
	    g_wrapped_key_size > BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE) {
		dev_err(&pdev->dev,
			"invalid qcom,wrapped-key-size %u, won't support generate/import/prepare hardware wrapped key\n",
			g_wrapped_key_size);
		g_wrapped_key_size = 0;
	}

	virtblk_set_crypto_ops(&virtblk_crypto_qcom_vops);
	return 0;
}

static void crypto_virt_remove(struct platform_device *pdev)
{
	virtblk_set_crypto_ops(NULL);
}

static const struct of_device_id crypto_virt_of_match[] = {
	{ .compatible = "qcom,crypto-virt" },
	{ }
};
MODULE_DEVICE_TABLE(of, crypto_virt_of_match);

static struct platform_driver crypto_virt_driver = {
	.probe = crypto_virt_probe,
	.remove = crypto_virt_remove,
	.driver = {
		.name = "crypto_virt",
		.of_match_table = crypto_virt_of_match,
	},
};

static int __init crypto_virt_init(void)
{
	return platform_driver_register(&crypto_virt_driver);
}
module_init(crypto_virt_init);

#if IS_MODULE(CONFIG_QCOM_CRYPTO_VIRT)
static void __exit crypto_virt_exit(void)
{
	platform_driver_unregister(&crypto_virt_driver);
}
module_exit(crypto_virt_exit);
#endif

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Crypto Virt Driver");
MODULE_LICENSE("GPL");
