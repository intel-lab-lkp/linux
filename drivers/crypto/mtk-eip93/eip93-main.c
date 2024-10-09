// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 - 2021
 *
 * Richard van Schagen <vschagen@icloud.com>
 * Christian Marangi <ansuelsmth@gmail.com
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <crypto/aes.h>
#include <crypto/ctr.h>

#include "eip93-main.h"
#include "eip93-regs.h"
#include "eip93-common.h"
#include "eip93-cipher.h"
#include "eip93-aes.h"
#include "eip93-des.h"
#include "eip93-aead.h"
#include "eip93-hash.h"

static struct mtk_alg_template *mtk_algs[] = {
	&mtk_alg_ecb_des,
	&mtk_alg_cbc_des,
	&mtk_alg_ecb_des3_ede,
	&mtk_alg_cbc_des3_ede,
	&mtk_alg_ecb_aes,
	&mtk_alg_cbc_aes,
	&mtk_alg_ctr_aes,
	&mtk_alg_rfc3686_aes,
	&mtk_alg_authenc_hmac_md5_cbc_des,
	&mtk_alg_authenc_hmac_sha1_cbc_des,
	&mtk_alg_authenc_hmac_sha224_cbc_des,
	&mtk_alg_authenc_hmac_sha256_cbc_des,
	&mtk_alg_authenc_hmac_md5_cbc_des3_ede,
	&mtk_alg_authenc_hmac_sha1_cbc_des3_ede,
	&mtk_alg_authenc_hmac_sha224_cbc_des3_ede,
	&mtk_alg_authenc_hmac_sha256_cbc_des3_ede,
	&mtk_alg_authenc_hmac_md5_cbc_aes,
	&mtk_alg_authenc_hmac_sha1_cbc_aes,
	&mtk_alg_authenc_hmac_sha224_cbc_aes,
	&mtk_alg_authenc_hmac_sha256_cbc_aes,
	&mtk_alg_authenc_hmac_md5_rfc3686_aes,
	&mtk_alg_authenc_hmac_sha1_rfc3686_aes,
	&mtk_alg_authenc_hmac_sha224_rfc3686_aes,
	&mtk_alg_authenc_hmac_sha256_rfc3686_aes,
	&mtk_alg_md5,
	&mtk_alg_sha1,
	&mtk_alg_sha224,
	&mtk_alg_sha256,
	&mtk_alg_hmac_md5,
	&mtk_alg_hmac_sha1,
	&mtk_alg_hmac_sha224,
	&mtk_alg_hmac_sha256,
};

inline void mtk_irq_disable(struct mtk_device *mtk, u32 mask)
{
	__raw_writel(mask, mtk->base + EIP93_REG_MASK_DISABLE);
}

inline void mtk_irq_enable(struct mtk_device *mtk, u32 mask)
{
	__raw_writel(mask, mtk->base + EIP93_REG_MASK_ENABLE);
}

inline void mtk_irq_clear(struct mtk_device *mtk, u32 mask)
{
	__raw_writel(mask, mtk->base + EIP93_REG_INT_CLR);
}

static void mtk_unregister_algs(unsigned int i)
{
	unsigned int j;

	for (j = 0; j < i; j++) {
		switch (mtk_algs[j]->type) {
		case MTK_ALG_TYPE_SKCIPHER:
			crypto_unregister_skcipher(&mtk_algs[j]->alg.skcipher);
			break;
		case MTK_ALG_TYPE_AEAD:
			crypto_unregister_aead(&mtk_algs[j]->alg.aead);
			break;
		case MTK_ALG_TYPE_HASH:
			crypto_unregister_ahash(&mtk_algs[i]->alg.ahash);
			break;
		}
	}
}

static int mtk_register_algs(struct mtk_device *mtk, u32 supported_algo_flags)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(mtk_algs); i++) {
		u32 alg_flags = mtk_algs[i]->flags;

		mtk_algs[i]->mtk = mtk;

		if ((IS_DES(alg_flags) || IS_3DES(alg_flags)) &&
		    !(supported_algo_flags & EIP93_PE_OPTION_TDES))
			continue;

		if (IS_AES(alg_flags)) {
			if (!(supported_algo_flags & EIP93_PE_OPTION_AES))
				continue;

			if (!IS_HMAC(alg_flags)) {
				if (supported_algo_flags & EIP93_PE_OPTION_AES_KEY128)
					mtk_algs[i]->alg.skcipher.max_keysize =
						AES_KEYSIZE_128;

				if (supported_algo_flags & EIP93_PE_OPTION_AES_KEY192)
					mtk_algs[i]->alg.skcipher.max_keysize =
						AES_KEYSIZE_192;

				if (supported_algo_flags & EIP93_PE_OPTION_AES_KEY256)
					mtk_algs[i]->alg.skcipher.max_keysize =
						AES_KEYSIZE_256;

				if (IS_RFC3686(alg_flags))
					mtk_algs[i]->alg.skcipher.max_keysize +=
						CTR_RFC3686_NONCE_SIZE;
			}
		}

		if (IS_HASH_MD5(alg_flags) &&
		    !(supported_algo_flags & EIP93_PE_OPTION_MD5))
			continue;

		if (IS_HASH_SHA1(alg_flags) &&
		    !(supported_algo_flags & EIP93_PE_OPTION_SHA_1))
			continue;

		if (IS_HASH_SHA224(alg_flags) &&
		    !(supported_algo_flags & EIP93_PE_OPTION_SHA_224))
			continue;

		if (IS_HASH_SHA256(alg_flags) &&
		    !(supported_algo_flags & EIP93_PE_OPTION_SHA_256))
			continue;

		switch (mtk_algs[i]->type) {
		case MTK_ALG_TYPE_SKCIPHER:
			ret = crypto_register_skcipher(&mtk_algs[i]->alg.skcipher);
			break;
		case MTK_ALG_TYPE_AEAD:
			ret = crypto_register_aead(&mtk_algs[i]->alg.aead);
			break;
		case MTK_ALG_TYPE_HASH:
			ret = crypto_register_ahash(&mtk_algs[i]->alg.ahash);
			break;
		}
		if (ret)
			goto fail;
	}

	return 0;

fail:
	mtk_unregister_algs(i);

	return ret;
}

static void mtk_handle_result_descriptor(struct mtk_device *mtk)
{
	struct crypto_async_request *async;
	struct eip93_descriptor *rdesc;
	u16 desc_flags, crypto_idr;
	bool last_entry;
	int handled, left, err;
	u32 pe_ctrl_stat;
	u32 pe_length;

get_more:
	handled = 0;

	left = readl(mtk->base + EIP93_REG_PE_RD_COUNT) & EIP93_PE_RD_COUNT;

	if (!left) {
		mtk_irq_clear(mtk, EIP93_INT_RDR_THRESH);
		mtk_irq_enable(mtk, EIP93_INT_RDR_THRESH);
		return;
	}

	last_entry = false;

	while (left) {
		rdesc = mtk_get_descriptor(mtk);
		if (IS_ERR(rdesc)) {
			dev_err(mtk->dev, "Ndesc: %d nreq: %d\n",
				handled, left);
			err = -EIO;
			break;
		}
		/* make sure DMA is finished writing */
		do {
			pe_ctrl_stat = READ_ONCE(rdesc->pe_ctrl_stat_word);
			pe_length = READ_ONCE(rdesc->pe_length_word);
		} while (FIELD_GET(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN, pe_ctrl_stat) !=
			 EIP93_PE_CTRL_PE_READY ||
			 FIELD_GET(EIP93_PE_LENGTH_HOST_PE_READY, pe_length) !=
			 EIP93_PE_LENGTH_PE_READY);

		err = rdesc->pe_ctrl_stat_word & (EIP93_PE_CTRL_PE_EXT_ERR_CODE |
						  EIP93_PE_CTRL_PE_EXT_ERR |
						  EIP93_PE_CTRL_PE_SEQNUM_ERR |
						  EIP93_PE_CTRL_PE_PAD_ERR |
						  EIP93_PE_CTRL_PE_AUTH_ERR);

		desc_flags = FIELD_GET(EIP93_PE_USER_ID_DESC_FLAGS, rdesc->user_id);
		crypto_idr = FIELD_GET(EIP93_PE_USER_ID_CRYPTO_IDR, rdesc->user_id);

		writel(1, mtk->base + EIP93_REG_PE_RD_COUNT);
		mtk_irq_clear(mtk, EIP93_INT_RDR_THRESH);

		handled++;
		left--;

		if (desc_flags & MTK_DESC_LAST) {
			last_entry = true;
			break;
		}
	}

	if (!last_entry)
		goto get_more;

	/* Get crypto async ref only for last descriptor */
	spin_lock_bh(&mtk->ring->idr_lock);
	async = idr_find(&mtk->ring->crypto_async_idr, crypto_idr);
	idr_remove(&mtk->ring->crypto_async_idr, crypto_idr);
	spin_unlock_bh(&mtk->ring->idr_lock);

	/* Parse error in ctrl stat word */
	err = mtk_parse_ctrl_stat_err(mtk, err);

	if (desc_flags & MTK_DESC_SKCIPHER)
		mtk_skcipher_handle_result(async, err);

	if (desc_flags & MTK_DESC_AEAD)
		mtk_aead_handle_result(async, err);

	if (desc_flags & MTK_DESC_HASH)
		mtk_hash_handle_result(async, err);

	goto get_more;
}

static void mtk_done_task(unsigned long data)
{
	struct mtk_device *mtk = (struct mtk_device *)data;

	mtk_handle_result_descriptor(mtk);
}

static irqreturn_t mtk_irq_handler(int irq, void *data)
{
	struct mtk_device *mtk = data;
	u32 irq_status;

	irq_status = readl(mtk->base + EIP93_REG_INT_MASK_STAT);
	if (FIELD_GET(EIP93_INT_RDR_THRESH, irq_status)) {
		mtk_irq_disable(mtk, EIP93_INT_RDR_THRESH);
		tasklet_schedule(&mtk->ring->done_task);
		return IRQ_HANDLED;
	}

	/* Ignore errors in AUTO mode, handled by the RDR */
	mtk_irq_clear(mtk, irq_status);
	if (irq_status)
		mtk_irq_disable(mtk, irq_status);

	return IRQ_NONE;
}

static void mtk_initialize(struct mtk_device *mtk, u32 supported_algo_flags)
{
	u32 val;

	/* Reset PE and rings */
	val = EIP93_PE_CONFIG_RST_PE | EIP93_PE_CONFIG_RST_RING;
	val |= EIP93_PE_TARGET_AUTO_RING_MODE;
	/* For Auto more, update the CDR ring owner after processing */
	val |= EIP93_PE_CONFIG_EN_CDR_UPDATE;
	writel(val, mtk->base + EIP93_REG_PE_CONFIG);

	/* Wait for PE and ring to reset */
	usleep_range(10, 20);

	/* Release PE and ring reset */
	val = readl(mtk->base + EIP93_REG_PE_CONFIG);
	val &= ~(EIP93_PE_CONFIG_RST_PE | EIP93_PE_CONFIG_RST_RING);
	writel(val, mtk->base + EIP93_REG_PE_CONFIG);

	/* Config Clocks */
	val = EIP93_PE_CLOCK_EN_PE_CLK;
	if (supported_algo_flags & EIP93_PE_OPTION_TDES)
		val |= EIP93_PE_CLOCK_EN_DES_CLK;
	if (supported_algo_flags & EIP93_PE_OPTION_AES)
		val |= EIP93_PE_CLOCK_EN_AES_CLK;
	if (supported_algo_flags &
	    (EIP93_PE_OPTION_MD5 | EIP93_PE_OPTION_SHA_1 | EIP93_PE_OPTION_SHA_224 |
	     EIP93_PE_OPTION_SHA_256))
		val |= EIP93_PE_CLOCK_EN_HASH_CLK;
	writel(val, mtk->base + EIP93_REG_PE_CLOCK_CTRL);

	/* Config DMA thresholds */
	val = FIELD_PREP(EIP93_PE_OUTBUF_THRESH, 128) |
	      FIELD_PREP(EIP93_PE_INBUF_THRESH, 128);
	writel(val, mtk->base + EIP93_REG_PE_BUF_THRESH);

	/* Clear/ack all interrupts before disable all */
	mtk_irq_clear(mtk, EIP93_INT_ALL);
	mtk_irq_disable(mtk, EIP93_INT_ALL);

	/* Setup CRD threshold to trigger interrupt */
	val = FIELD_PREP(EIPR93_PE_CDR_THRESH, MTK_RING_NUM - MTK_RING_BUSY);
	/*
	 * Configure RDR interrupt to be triggered if RD counter is not 0
	 * for more than 2^(N+10) system clocks.
	 */
	val |= FIELD_PREP(EIPR93_PE_RD_TIMEOUT, 5) | EIPR93_PE_TIMEROUT_EN;
	writel(val, mtk->base + EIP93_REG_PE_RING_THRESH);
}

static void mtk_desc_free(struct mtk_device *mtk)
{
	writel(0, mtk->base + EIP93_REG_PE_RING_CONFIG);
	writel(0, mtk->base + EIP93_REG_PE_CDR_BASE);
	writel(0, mtk->base + EIP93_REG_PE_RDR_BASE);
}

static int mtk_set_ring(struct mtk_device *mtk, struct mtk_desc_ring *ring)
{
	ring->offset = sizeof(struct eip93_descriptor);
	ring->base = dmam_alloc_coherent(mtk->dev,
					 sizeof(struct eip93_descriptor) * MTK_RING_NUM,
					 &ring->base_dma, GFP_KERNEL);
	if (!ring->base)
		return -ENOMEM;

	ring->write = ring->base;
	ring->base_end = ring->base + sizeof(struct eip93_descriptor) * (MTK_RING_NUM - 1);
	ring->read  = ring->base;

	return 0;
}

static int mtk_desc_init(struct mtk_device *mtk)
{
	struct mtk_desc_ring *cdr = &mtk->ring->cdr;
	struct mtk_desc_ring *rdr = &mtk->ring->rdr;
	int ret;
	u32 val;

	ret = mtk_set_ring(mtk, cdr);
	if (ret)
		return ret;

	ret = mtk_set_ring(mtk, rdr);
	if (ret)
		return ret;

	writel((u32 __force)cdr->base_dma, mtk->base + EIP93_REG_PE_CDR_BASE);
	writel((u32 __force)rdr->base_dma, mtk->base + EIP93_REG_PE_RDR_BASE);

	val = FIELD_PREP(EIP93_PE_RING_SIZE, MTK_RING_NUM - 1);
	writel(val, mtk->base + EIP93_REG_PE_RING_CONFIG);

	atomic_set(&mtk->ring->free, MTK_RING_NUM - 1);

	return 0;
}

static void mtk_cleanup(struct mtk_device *mtk)
{
	tasklet_kill(&mtk->ring->done_task);

	/* Clear/ack all interrupts before disable all */
	mtk_irq_clear(mtk, EIP93_INT_ALL);
	mtk_irq_disable(mtk, EIP93_INT_ALL);

	writel(0, mtk->base + EIP93_REG_PE_CLOCK_CTRL);

	mtk_desc_free(mtk);

	idr_destroy(&mtk->ring->crypto_async_idr);
}

static int mtk_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_device *mtk;
	u32 ver, algo_flags;
	int ret;

	mtk = devm_kzalloc(dev, sizeof(*mtk), GFP_KERNEL);
	if (!mtk)
		return -ENOMEM;

	mtk->dev = dev;
	platform_set_drvdata(pdev, mtk);

	mtk->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mtk->base))
		return PTR_ERR(mtk->base);

	mtk->irq = platform_get_irq(pdev, 0);
	if (mtk->irq < 0)
		return mtk->irq;

	ret = devm_request_threaded_irq(mtk->dev, mtk->irq, mtk_irq_handler,
					NULL, IRQF_ONESHOT,
					dev_name(mtk->dev), mtk);

	mtk->ring = devm_kcalloc(mtk->dev, 1, sizeof(*mtk->ring), GFP_KERNEL);
	if (!mtk->ring)
		return -ENOMEM;

	ret = mtk_desc_init(mtk);

	if (ret)
		return ret;

	tasklet_init(&mtk->ring->done_task, mtk_done_task, (unsigned long)mtk);

	spin_lock_init(&mtk->ring->read_lock);
	spin_lock_init(&mtk->ring->write_lock);

	spin_lock_init(&mtk->ring->idr_lock);
	idr_init(&mtk->ring->crypto_async_idr);

	algo_flags = readl(mtk->base + EIP93_REG_PE_OPTION_1);

	mtk_initialize(mtk, algo_flags);

	/* Init finished, enable RDR interrupt */
	mtk_irq_enable(mtk, EIP93_INT_RDR_THRESH);

	ret = mtk_register_algs(mtk, algo_flags);
	if (ret) {
		mtk_cleanup(mtk);
		return ret;
	}

	ver = readl(mtk->base + EIP93_REG_PE_REVISION);
	/* EIP_EIP_NO:MAJOR_HW_REV:MINOR_HW_REV:HW_PATCH,PE(ALGO_FLAGS) */
	dev_info(mtk->dev, "EIP%lu:%lx:%lx:%lx,PE(0x%x:0x%x)\n",
		 FIELD_GET(EIP93_PE_REVISION_EIP_NO, ver),
		 FIELD_GET(EIP93_PE_REVISION_MAJ_HW_REV, ver),
		 FIELD_GET(EIP93_PE_REVISION_MIN_HW_REV, ver),
		 FIELD_GET(EIP93_PE_REVISION_HW_PATCH, ver),
		 algo_flags,
		 readl(mtk->base + EIP93_REG_PE_OPTION_0));

	return 0;
}

static int mtk_crypto_remove(struct platform_device *pdev)
{
	struct mtk_device *mtk = platform_get_drvdata(pdev);

	mtk_unregister_algs(ARRAY_SIZE(mtk_algs));

	mtk_cleanup(mtk);

	dev_info(mtk->dev, "EIP93 removed.\n");

	return 0;
}

static const struct of_device_id mtk_crypto_of_match[] = {
	{ .compatible = "mediatek,mtk-eip93", },
	{ .compatible = "airoha,mtk-eip93", },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_crypto_of_match);

static struct platform_driver mtk_crypto_driver = {
	.probe = mtk_crypto_probe,
	.remove = mtk_crypto_remove,
	.driver = {
		.name = "mtk-eip93",
		.of_match_table = mtk_crypto_of_match,
	},
};
module_platform_driver(mtk_crypto_driver);

MODULE_AUTHOR("Richard van Schagen <vschagen@cs.com>");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Mediatek EIP-93 crypto engine driver");
MODULE_LICENSE("GPL");
