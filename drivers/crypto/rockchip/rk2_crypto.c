// SPDX-License-Identifier: GPL-2.0
/*
 * hardware cryptographic offloader for RK3568/RK3588 SoC
 *
 * Copyright (c) 2022-2023, Corentin Labbe <clabbe@baylibre.com>
 */

#include "rk2_crypto.h"
#include <linux/clk.h>
#include <linux/crypto.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <crypto/aes.h>

static const struct rk2_variant rk3568_variant = {
	.num_clks = 3,
};

static const struct rk2_variant rk3588_variant = {
	.num_clks = 3,
};

static int rk2_crypto_get_clks(struct rk2_crypto_dev *dev)
{
	dev->num_clks = devm_clk_bulk_get_all(dev->dev, &dev->clks);
	if (dev->num_clks < 0)
		return dev_err_probe(dev->dev, dev->num_clks, "Failed to get clocks\n");
	if (dev->num_clks < dev->variant->num_clks)
		return dev_err_probe(dev->dev, -EINVAL,
				"Missing clocks, got %d instead of %d\n",
				dev->num_clks, dev->variant->num_clks);
	return 0;
}

static int rk2_crypto_pm_suspend(struct device *dev)
{
	struct rk2_crypto_dev *rkdev = dev_get_drvdata(dev);

	reset_control_assert(rkdev->rst);
	udelay(10);
	clk_bulk_disable_unprepare(rkdev->num_clks, rkdev->clks);

	return 0;
}

static int rk2_crypto_pm_resume(struct device *dev)
{
	struct rk2_crypto_dev *rkdev = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(rkdev->num_clks, rkdev->clks);
	if (ret)
		return ret;

	udelay(10);
	reset_control_deassert(rkdev->rst);

	return 0;
}

static const struct dev_pm_ops rk2_crypto_pm_ops = {
	RUNTIME_PM_OPS(rk2_crypto_pm_suspend, rk2_crypto_pm_resume, NULL)
};

static int rk2_crypto_pm_init(struct rk2_crypto_dev *rkdev)
{
	int err;

	pm_runtime_use_autosuspend(rkdev->dev);
	pm_runtime_set_autosuspend_delay(rkdev->dev, 2000);

	err = pm_runtime_set_suspended(rkdev->dev);
	if (err)
		return err;
	pm_runtime_enable(rkdev->dev);

	return 0;
}

static void rk2_crypto_pm_exit(struct rk2_crypto_dev *rkdev)
{
	pm_runtime_disable(rkdev->dev);
}

static irqreturn_t rk2_crypto_irq_handle(int irq, void *dev_id)
{
	struct rk2_crypto_dev *rkc = platform_get_drvdata(dev_id);
	u32 v;

	v = readl(rkc->reg + RK2_CRYPTO_DMA_INT_ST);
	if (!v)
		return IRQ_NONE;

	writel(v, rkc->reg + RK2_CRYPTO_DMA_INT_ST);

	/*
	 * Only signal completion on list-done or hard DMA error.
	 * Intermediate SRC_INT (BIT(1)/BIT(2)) fire for every LLI
	 * entry that has RK2_LLI_DMA_CTRL_SRC_INT set. Completing
	 * early on those causes the driver to read hash registers
	 * before all data has been processed, producing wrong results.
	 */
	if (v & RK2_CRYPTO_DMA_INT_ERR_MASK) {
		dev_warn(rkc->dev, "DMA Error\n");
		rkc->status = 0;
		complete(&rkc->complete);
	} else if (v & RK2_CRYPTO_DMA_INT_LISTDONE) {
		rkc->status = 1;
		complete(&rkc->complete);
	}

	return IRQ_HANDLED;
}

static const struct rk2_crypto_template rk2_crypto_algs_template[] = {
	{
	.type = CRYPTO_ALG_TYPE_SKCIPHER,
	.rk2_mode = RK2_CRYPTO_AES_ECB,
	.alg.skcipher.base = {
			.base.cra_name = "ecb(aes)",
			.base.cra_driver_name = "ecb-aes-rk2",
			.base.cra_priority = 300,
			.base.cra_flags =
			CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize = AES_BLOCK_SIZE,
			.base.cra_ctxsize = sizeof(struct rk2_cipher_ctx),
			.base.cra_alignmask = 0,
			.base.cra_module = THIS_MODULE,
			.init = rk2_cipher_tfm_init,
			.exit = rk2_cipher_tfm_exit,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.setkey = rk2_aes_setkey,
			.encrypt = rk2_skcipher_encrypt,
			.decrypt = rk2_skcipher_decrypt,
			},
	.alg.skcipher.op = {
		.do_one_request = rk2_cipher_run,
		},
	},
	{
	 .type = CRYPTO_ALG_TYPE_SKCIPHER,
	 .rk2_mode = RK2_CRYPTO_AES_CBC,
	 .alg.skcipher.base = {
			.base.cra_name = "cbc(aes)",
			.base.cra_driver_name = "cbc-aes-rk2",
			.base.cra_priority = 300,
			.base.cra_flags =
			CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize = AES_BLOCK_SIZE,
			.base.cra_ctxsize =
			sizeof(struct rk2_cipher_ctx),
			.base.cra_alignmask = 0,
			.base.cra_module = THIS_MODULE,
			.init = rk2_cipher_tfm_init,
			.exit = rk2_cipher_tfm_exit,
			.min_keysize = AES_MIN_KEY_SIZE,
			.max_keysize = AES_MAX_KEY_SIZE,
			.ivsize = AES_BLOCK_SIZE,
			.setkey = rk2_aes_setkey,
			.encrypt = rk2_skcipher_encrypt,
			.decrypt = rk2_skcipher_decrypt,
			},
	.alg.skcipher.op = {
		.do_one_request = rk2_cipher_run,
		},
	},
	{
	.type = CRYPTO_ALG_TYPE_SKCIPHER,
	.rk2_mode = RK2_CRYPTO_AES_XTS,
	.is_xts = true,
	.alg.skcipher.base = {
			.base.cra_name = "xts(aes)",
			.base.cra_driver_name = "xts-aes-rk2",
			.base.cra_priority = 300,
			.base.cra_flags =
			CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.base.cra_blocksize = AES_BLOCK_SIZE,
			.base.cra_ctxsize =
			sizeof(struct rk2_cipher_ctx),
			.base.cra_alignmask = 0,
			.base.cra_module = THIS_MODULE,
			.init = rk2_cipher_tfm_init,
			.exit = rk2_cipher_tfm_exit,
			.min_keysize = AES_MIN_KEY_SIZE * 2,
			.max_keysize = AES_MAX_KEY_SIZE * 2,
			.ivsize = AES_BLOCK_SIZE,
			.setkey = rk2_aes_xts_setkey,
			.encrypt = rk2_skcipher_encrypt,
			.decrypt = rk2_skcipher_decrypt,
			},
	.alg.skcipher.op = {
		.do_one_request = rk2_cipher_run,
		},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_MD5,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = MD5_DIGEST_SIZE,
				.statesize = sizeof(struct md5_state),
				.base = {
					.cra_name = "md5",
					.cra_driver_name = "rk2-md5",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize =
					MD5_HMAC_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SHA1,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SHA1_DIGEST_SIZE,
				.statesize = sizeof(struct sha1_state),
				.base = {
					.cra_name = "sha1",
					.cra_driver_name = "rk2-sha1",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA1_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SHA224,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SHA224_DIGEST_SIZE,
				.statesize = sizeof(struct sha256_state),
				.base = {
					.cra_name = "sha224",
					.cra_driver_name = "rk2-sha224",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA256_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SHA256,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SHA256_DIGEST_SIZE,
				.statesize = sizeof(struct sha256_state),
				.base = {
					.cra_name = "sha256",
					.cra_driver_name = "rk2-sha256",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA256_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SHA384,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SHA384_DIGEST_SIZE,
				.statesize = sizeof(struct sha512_state),
				.base = {
					.cra_name = "sha384",
					.cra_driver_name = "rk2-sha384",
					.cra_priority = 300,
					.cra_flags = CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA384_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SHA512,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SHA512_DIGEST_SIZE,
				.statesize = sizeof(struct sha512_state),
				.base = {
					.cra_name = "sha512",
					.cra_driver_name = "rk2-sha512",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SHA512_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
	{
	.type = CRYPTO_ALG_TYPE_AHASH,
	.rk2_mode = RK2_CRYPTO_SM3,
	.alg.hash.base = {
			.init = rk2_ahash_init,
			.update = rk2_ahash_update,
			.final = rk2_ahash_final,
			.finup = rk2_ahash_finup,
			.export = rk2_ahash_export,
			.import = rk2_ahash_import,
			.digest = rk2_ahash_digest,
			.init_tfm = rk2_hash_init_tfm,
			.exit_tfm = rk2_hash_exit_tfm,
			.halg = {
				.digestsize = SM3_DIGEST_SIZE,
				.statesize = sizeof(struct sm3_state),
				.base = {
					.cra_name = "sm3",
					.cra_driver_name = "rk2-sm3",
					.cra_priority = 300,
					.cra_flags =
					CRYPTO_ALG_ASYNC |
					CRYPTO_ALG_NEED_FALLBACK,
					.cra_blocksize = SM3_BLOCK_SIZE,
					.cra_ctxsize =
					sizeof(struct rk2_ahash_ctx),
					.cra_module = THIS_MODULE,
					}
				}
			},
	.alg.hash.op = {
			.do_one_request = rk2_hash_run,
			},
	},
};

#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
static int rk2_crypto_debugfs_stats_show(struct seq_file *seq, void *v)
{
	struct rk2_crypto_dev *rkc = seq->private;
	unsigned int i;

	seq_printf(seq, "%s %s requests: %lu\n",
		   dev_driver_string(rkc->dev), dev_name(rkc->dev), rkc->nreq);

	for (i = 0; i < rkc->num_algs; i++) {
		switch (rkc->algs[i].type) {
		case CRYPTO_ALG_TYPE_SKCIPHER:
			seq_printf(seq, "%s %s reqs=%lu fallback=%lu\n",
				   rkc->algs[i].alg.skcipher.base.base.cra_driver_name,
				   rkc->algs[i].alg.skcipher.base.base.cra_name,
				   rkc->algs[i].stat_req, rkc->algs[i].stat_fb);
			seq_printf(seq, "\tfallback due to length: %lu\n",
				   rkc->algs[i].stat_fb_len);
			seq_printf(seq, "\tfallback due to alignment: %lu\n",
				   rkc->algs[i].stat_fb_align);
			seq_printf(seq, "\tfallback due to SGs: %lu\n",
				   rkc->algs[i].stat_fb_sgdiff);
			break;
		case CRYPTO_ALG_TYPE_AHASH:
			seq_printf(seq, "%s %s reqs=%lu fallback=%lu\n",
				   rkc->algs[i].alg.hash.base.halg.base.cra_driver_name,
				   rkc->algs[i].alg.hash.base.halg.base.cra_name,
				   rkc->algs[i].stat_req,
				   rkc->algs[i].stat_fb);
			break;
		}
	}
	return 0;
}

static int rk2_crypto_debugfs_info_show(struct seq_file *seq, void *d)
{
	struct rk2_crypto_dev *rkc = seq->private;
	u32 v;
	int err;

	err = pm_runtime_resume_and_get(rkc->dev);
	if (err)
		return err;

	v = readl(rkc->reg + RK2_CRYPTO_CLK_CTL);
	seq_printf(seq, "CRYPTO_CLK_CTL %x\n", v);
	v = readl(rkc->reg + RK2_CRYPTO_RST_CTL);
	seq_printf(seq, "CRYPTO_RST_CTL %x\n", v);
	v = readl(rkc->reg + CRYPTO_AES_VERSION);
	seq_printf(seq, "CRYPTO_AES_VERSION %x\n", v);
	if (v & BIT(17))
		seq_puts(seq, "AES 192\n");
	v = readl(rkc->reg + CRYPTO_DES_VERSION);
	seq_printf(seq, "CRYPTO_DES_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_SM4_VERSION);
	seq_printf(seq, "CRYPTO_SM4_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_HASH_VERSION);
	seq_printf(seq, "CRYPTO_HASH_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_HMAC_VERSION);
	seq_printf(seq, "CRYPTO_HMAC_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_RNG_VERSION);
	seq_printf(seq, "CRYPTO_RNG_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_PKA_VERSION);
	seq_printf(seq, "CRYPTO_PKA_VERSION %x\n", v);
	v = readl(rkc->reg + CRYPTO_CRYPTO_VERSION);
	seq_printf(seq, "CRYPTO_CRYPTO_VERSION %x\n", v);

	pm_runtime_mark_last_busy(rkc->dev);
	pm_runtime_put_autosuspend(rkc->dev);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(rk2_crypto_debugfs_stats);
DEFINE_SHOW_ATTRIBUTE(rk2_crypto_debugfs_info);

#endif

static void register_debugfs(struct rk2_crypto_dev *rkc)
{
#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
	/* Create a directory using the device name
	 * (e.g., /sys/kernel/debug/fe370000.crypto)
	 */
	rkc->dbgfs_dir = debugfs_create_dir(dev_name(rkc->dev), NULL);

	debugfs_create_file("stats", 0440, rkc->dbgfs_dir, rkc,
			    &rk2_crypto_debugfs_stats_fops);
	debugfs_create_file("info", 0440, rkc->dbgfs_dir, rkc,
			    &rk2_crypto_debugfs_info_fops);
#endif
}

static int rk2_crypto_register(struct rk2_crypto_dev *rkc)
{
	int i, k, err = 0;

	for (i = 0; i < rkc->num_algs; i++) {
		rkc->algs[i].dev = rkc;	/* Tie this alg copy to this device */
		switch (rkc->algs[i].type) {
		case CRYPTO_ALG_TYPE_SKCIPHER:
			err = crypto_engine_register_skcipher(&rkc->algs[i].alg.skcipher);
			break;
		case CRYPTO_ALG_TYPE_AHASH:
			err = crypto_engine_register_ahash(&rkc->algs[i].alg.hash);
			break;
		}
		if (err)
			goto err_cipher_algs;
	}
	return 0;

 err_cipher_algs:
	for (k = 0; k < i; k++) {
		if (rkc->algs[k].type == CRYPTO_ALG_TYPE_SKCIPHER)
			crypto_engine_unregister_skcipher(&rkc->algs[k].alg.skcipher);
		else
			crypto_engine_unregister_ahash(&rkc->algs[k].alg.hash);
	}
	return err;
}

static void rk2_crypto_unregister(struct rk2_crypto_dev *rkc)
{
	int i;

	for (i = 0; i < rkc->num_algs; i++) {
		if (rkc->algs[i].type == CRYPTO_ALG_TYPE_SKCIPHER)
			crypto_engine_unregister_skcipher(&rkc->algs[i].alg.skcipher);
		else
			crypto_engine_unregister_ahash(&rkc->algs[i].alg.hash);
	}
}

static const struct of_device_id crypto_of_id_table[] = {
	{.compatible = "rockchip,rk3568-crypto",
	 .data = &rk3568_variant,
	 },
	{.compatible = "rockchip,rk3588-crypto",
	 .data = &rk3588_variant,
	 },
	{}
};

MODULE_DEVICE_TABLE(of, crypto_of_id_table);

static int rk2_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk2_crypto_dev *rkc;
	int err = 0;

	rkc = devm_kzalloc(dev, sizeof(*rkc), GFP_KERNEL);
	if (!rkc)
		return -ENOMEM;

	rkc->dev = dev;
	platform_set_drvdata(pdev, rkc);

	/* Duplicate the algorithms locally for this specific device */
	rkc->num_algs = ARRAY_SIZE(rk2_crypto_algs_template);
	rkc->algs = devm_kmemdup(dev, rk2_crypto_algs_template,
				 sizeof(rk2_crypto_algs_template), GFP_KERNEL);
	if (!rkc->algs)
		return -ENOMEM;

	rkc->variant = of_device_get_match_data(dev);
	if (!rkc->variant)
		return dev_err_probe(dev, -EINVAL, "Missing variant\n");

	rkc->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rkc->rst))
		return dev_err_probe(dev, PTR_ERR(rkc->rst), "Fail to get resets\n");

	/* Manual DMA allocation requires manual cleanup in error paths */
	rkc->tl = dma_alloc_coherent(dev,
				     sizeof(struct rk2_crypto_lli) * MAX_LLI,
				     &rkc->t_phy, GFP_KERNEL);

	if (!rkc->tl)
		return -ENOMEM;

	rkc->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkc->reg)) {
		err = dev_err_probe(dev, PTR_ERR(rkc->reg), "Fail to get resources\n");
		goto err_dma;
	}

	err = rk2_crypto_get_clks(rkc);
	if (err)
		goto err_dma;

	rkc->irq = platform_get_irq(pdev, 0);
	if (rkc->irq < 0) {
		err = dev_err_probe(dev, rkc->irq, "Interrupt is not available.\n");
		goto err_dma;
	}

	err = devm_request_irq(dev, rkc->irq,
			       rk2_crypto_irq_handle, IRQF_SHARED,
			       "rk-crypto", pdev);

	if (err) {
		err = dev_err_probe(dev, err, "irq request failed.\n");
		goto err_dma;
	}

	rkc->engine = crypto_engine_alloc_init(dev, true);
	if (!rkc->engine) {
		err = -ENOMEM;
		goto err_dma;
	}

	err = crypto_engine_start(rkc->engine);
	if (err) {
		err = dev_err_probe(dev, err, "Failed to start crypto engine\n");
		goto err_engine;
	}

	init_completion(&rkc->complete);

	err = rk2_crypto_pm_init(rkc);
	if (err) {
		err = dev_err_probe(dev, err, "Failed to initialize runtime PM\n");
		goto err_engine;
	}

	err = pm_runtime_resume_and_get(dev);
	if (err) {
		err = dev_err_probe(dev, err, "Failed to resume device\n");
		goto err_pm;
	}

	/* Register algorithms specific to THIS device */
	err = rk2_crypto_register(rkc);
	if (err) {
		err = dev_err_probe(dev, err, "Fail to register crypto algorithms\n");
		goto err_pm_put;
	}

	register_debugfs(rkc);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

 err_pm_put:
	pm_runtime_put_sync(dev);
 err_pm:
	rk2_crypto_pm_exit(rkc);
 err_engine:
	crypto_engine_exit(rkc->engine);
 err_dma:
	dma_free_coherent(dev, sizeof(struct rk2_crypto_lli) * MAX_LLI,
			  rkc->tl, rkc->t_phy);
	return err;
}

static void rk2_crypto_remove(struct platform_device *pdev)
{
	struct rk2_crypto_dev *rkc = platform_get_drvdata(pdev);

	/* Stop engine to prevent new requests */
	crypto_engine_stop(rkc->engine);

	/* Unregister algorithms for this specific device */
	rk2_crypto_unregister(rkc);

#ifdef CONFIG_CRYPTO_DEV_ROCKCHIP2_DEBUG
	debugfs_remove_recursive(rkc->dbgfs_dir);
#endif

	/* Safe to kill the engine completely */
	crypto_engine_exit(rkc->engine);

	rk2_crypto_pm_exit(rkc);
	dma_free_coherent(rkc->dev, sizeof(struct rk2_crypto_lli) * MAX_LLI,
			  rkc->tl, rkc->t_phy);
}

static struct platform_driver crypto_driver = {
	.probe = rk2_crypto_probe,
	.remove = rk2_crypto_remove,
	.driver = {
		   .name = "rk2-crypto",
		   .pm = pm_ptr(&rk2_crypto_pm_ops),
		   .of_match_table = crypto_of_id_table,
		   },
};

module_platform_driver(crypto_driver);

MODULE_DESCRIPTION("Rockchip Crypto Engine cryptographic offloader");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Corentin Labbe <clabbe@baylibre.com>");
MODULE_AUTHOR("Dawid Olesinski <dawidro@gmail.com>");

