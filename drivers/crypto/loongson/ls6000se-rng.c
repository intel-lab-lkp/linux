// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019 HiSilicon Limited. */
/* Copyright (c) 2025 Loongson Technology Corporation Limited. */

#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mfd/ls6000se.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <crypto/internal/rng.h>

struct lsrng_list {
	struct mutex lock;
	struct list_head list;
	int is_init;
};

struct lsrng {
	bool is_used;
	struct lsse_ch *se_ch;
	struct list_head list;
	struct completion rng_completion;
};

struct lsrng_ctx {
	struct lsrng *rng;
};

struct rng_msg {
	u32 cmd;
	union {
		u32 len;
		u32 ret;
	} u;
	u32 resved;
	u32 out_off;
	u32 pad[4];
};

static atomic_t rng_active_devs;
static struct lsrng_list rng_devices;

static void lsrng_complete(struct lsse_ch *ch)
{
	struct lsrng *rng = (struct lsrng *)ch->priv;

	complete(&rng->rng_completion);
}

static int lsrng_generate(struct crypto_rng *tfm, const u8 *src,
			  unsigned int slen, u8 *dstn, unsigned int dlen)
{
	struct lsrng_ctx *ctx = crypto_rng_ctx(tfm);
	struct lsrng *rng = ctx->rng;
	struct rng_msg *msg;
	int err, len;

	do {
		len = min(dlen, PAGE_SIZE);
		msg = rng->se_ch->smsg;
		msg->u.len = len;
		err = se_send_ch_requeset(rng->se_ch);
		if (err)
			return err;

		wait_for_completion_interruptible(&rng->rng_completion);

		msg = rng->se_ch->rmsg;
		if (msg->u.ret)
			return -EFAULT;

		memcpy(dstn, rng->se_ch->data_buffer, len);
		dlen -= len;
		dstn += len;
	} while (dlen > 0);

	return 0;
}

static int lsrng_init(struct crypto_tfm *tfm)
{
	struct lsrng_ctx *ctx = crypto_tfm_ctx(tfm);
	struct lsrng *rng;
	int ret = -EBUSY;

	mutex_lock(&rng_devices.lock);
	list_for_each_entry(rng, &rng_devices.list, list) {
		if (!rng->is_used) {
			rng->is_used = true;
			ctx->rng = rng;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&rng_devices.lock);

	return ret;
}

static void lsrng_exit(struct crypto_tfm *tfm)
{
	struct lsrng_ctx *ctx = crypto_tfm_ctx(tfm);

	mutex_lock(&rng_devices.lock);
	ctx->rng->is_used = false;
	mutex_unlock(&rng_devices.lock);
}

static int no_seed(struct crypto_rng *tfm, const u8 *seed, unsigned int slen)
{
	return 0;
}

static struct rng_alg lsrng_alg = {
	.generate = lsrng_generate,
	.seed =	no_seed,
	.base = {
		.cra_name = "stdrng",
		.cra_driver_name = "loongson_stdrng",
		.cra_priority = 300,
		.cra_ctxsize = sizeof(struct lsrng_ctx),
		.cra_module = THIS_MODULE,
		.cra_init = lsrng_init,
		.cra_exit = lsrng_exit,
	},
};

static void lsrng_add_to_list(struct lsrng *rng)
{
	mutex_lock(&rng_devices.lock);
	list_add_tail(&rng->list, &rng_devices.list);
	mutex_unlock(&rng_devices.lock);
}

static int lsrng_probe(struct platform_device *pdev)
{
	struct rng_msg *msg;
	struct lsrng *rng;
	int ret;

	rng = devm_kzalloc(&pdev->dev, sizeof(*rng), GFP_KERNEL);
	if (!rng)
		return -ENOMEM;

	init_completion(&rng->rng_completion);
	rng->se_ch = se_init_ch(pdev->dev.parent, SE_CH_RNG, PAGE_SIZE,
				sizeof(struct rng_msg) * 2, rng, lsrng_complete);
	if (!rng->se_ch)
		return -ENODEV;
	msg = rng->se_ch->smsg;
	msg->cmd = SE_CMD_RNG;
	msg->out_off = rng->se_ch->off;

	if (!rng_devices.is_init) {
		ret = crypto_register_rng(&lsrng_alg);
		if (ret) {
			dev_err(&pdev->dev, "failed to register crypto(%d)\n", ret);
			return ret;
		}
		INIT_LIST_HEAD(&rng_devices.list);
		mutex_init(&rng_devices.lock);
		rng_devices.is_init = true;
	}

	lsrng_add_to_list(rng);
	atomic_inc(&rng_active_devs);

	return 0;
}

static struct platform_driver lsrng_driver = {
	.probe		= lsrng_probe,
	.driver		= {
		.name	= "ls6000se-rng",
	},
};
module_platform_driver(lsrng_driver);

MODULE_ALIAS("platform:ls6000se-rng");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yinggang Gu <guyinggang@loongson.cn>");
MODULE_AUTHOR("Qunqin Zhao <zhaoqunqin@loongson.cn>");
MODULE_DESCRIPTION("Loongson random number generator driver");
