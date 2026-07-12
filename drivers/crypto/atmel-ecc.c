// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip / Atmel ECC (I2C) driver.
 *
 * Copyright (c) 2017, Microchip Technology Inc.
 * Author: Tudor Ambarus
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <crypto/internal/kpp.h>
#include <crypto/ecdh.h>
#include <crypto/kpp.h>
#include "atmel-i2c.h"

static DEFINE_MUTEX(atmel_ecc_kpp_lock);
static int atmel_ecc_kpp_refcnt;
DECLARE_COMPLETION(atmel_ecc_unreg_done);
static bool atmel_ecc_unreg_active;

static struct atmel_ecc_driver_data driver_data;

/**
 * struct atmel_ecdh_ctx - transformation context
 * @client: I2C client device
 * @fallback: ECDH fallback used for caller-provided private keys
 * @public_key: cached public key for the device-generated private key
 * @do_fallback: true when ECDH operations should use @fallback
 *
 * The caller must not invoke set_secret() while generate_public_key()
 * or compute_shared_secret() are in flight.
 */
struct atmel_ecdh_ctx {
	struct i2c_client *client;
	struct crypto_kpp *fallback;
	const u8 *public_key;
	bool do_fallback;
};

static void atmel_ecdh_done(struct atmel_i2c_work_data *work_data, void *areq,
			    int status)
{
	struct kpp_request *req = areq;
	struct atmel_i2c_cmd *cmd = &work_data->cmd;
	size_t copied, n_sz;

	if (status)
		goto free_work_data;

	/* copy only as much as requested, capped at 32 bytes */
	n_sz = min(ATMEL_ECC_NIST_P256_N_SIZE, req->dst_len);

	/* copy the shared secret */
	copied = sg_copy_from_buffer(req->dst, sg_nents_for_len(req->dst, n_sz),
				     &cmd->data[RSP_DATA_IDX], n_sz);
	if (copied != n_sz)
		status = -EINVAL;

free_work_data:
	kfree_sensitive(work_data);
	kpp_request_complete(req, status);
}

/*
 * If no private key is provided, generate one in the device and cache
 * the corresponding public key. The generated private key never leaves
 * the device.
 */
static int atmel_ecdh_set_secret(struct crypto_kpp *tfm, const void *buf,
				 unsigned int len)
{
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);
	struct atmel_i2c_cmd *cmd;
	void *public_key;
	struct ecdh params;
	int ret = -ENOMEM;

	kfree(ctx->public_key);
	ctx->public_key = NULL;

	if (crypto_ecdh_decode_key(buf, len, &params) < 0) {
		dev_err(&ctx->client->dev, "crypto_ecdh_decode_key failed\n");
		return -EINVAL;
	}

	if (params.key_size) {
		ctx->do_fallback = true;
		return crypto_kpp_set_secret(ctx->fallback, buf, len);
	}

	cmd = kmalloc_obj(*cmd);
	if (!cmd)
		return -ENOMEM;

	public_key = kmalloc(ATMEL_ECC_PUBKEY_SIZE, GFP_KERNEL);
	if (!public_key)
		goto free_cmd;

	ctx->do_fallback = false;

	atmel_i2c_init_genkey_cmd(cmd, DATA_SLOT_2);

	ret = atmel_i2c_send_receive(ctx->client, cmd);
	if (ret)
		goto free_public_key;

	memcpy(public_key, &cmd->data[RSP_DATA_IDX], ATMEL_ECC_PUBKEY_SIZE);
	ctx->public_key = public_key;

	kfree(cmd);
	return 0;

free_public_key:
	kfree(public_key);
free_cmd:
	kfree(cmd);
	return ret;
}

static int atmel_ecdh_generate_public_key(struct kpp_request *req)
{
	struct crypto_kpp *tfm = crypto_kpp_reqtfm(req);
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);
	size_t copied, nbytes;
	int ret = 0;

	if (ctx->do_fallback) {
		kpp_request_set_tfm(req, ctx->fallback);
		return crypto_kpp_generate_public_key(req);
	}

	if (!ctx->public_key)
		return -EINVAL;

	/* copy only as much as requested, capped at 64 bytes */
	nbytes = min(ATMEL_ECC_PUBKEY_SIZE, req->dst_len);

	/* public key was saved at private key generation */
	copied = sg_copy_from_buffer(req->dst,
				     sg_nents_for_len(req->dst, nbytes),
				     ctx->public_key, nbytes);
	if (copied != nbytes)
		ret = -EINVAL;

	return ret;
}

static int atmel_ecdh_compute_shared_secret(struct kpp_request *req)
{
	struct crypto_kpp *tfm = crypto_kpp_reqtfm(req);
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);
	struct atmel_i2c_work_data *work_data;
	gfp_t gfp;
	int ret;

	if (ctx->do_fallback) {
		kpp_request_set_tfm(req, ctx->fallback);
		return crypto_kpp_compute_shared_secret(req);
	}

	if (!ctx->public_key)
		return -EINVAL;

	/* A P-256 public key must contain two 32-byte coordinates */
	if (req->src_len != ATMEL_ECC_PUBKEY_SIZE)
		return -EINVAL;

	gfp = (req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ? GFP_KERNEL :
							     GFP_ATOMIC;

	work_data = kmalloc_obj(*work_data, gfp);
	if (!work_data)
		return -ENOMEM;

	work_data->ctx = ctx;
	work_data->client = ctx->client;

	ret = atmel_i2c_init_ecdh_cmd(&work_data->cmd, req->src);
	if (ret)
		goto free_work_data;

	atmel_i2c_enqueue(work_data, atmel_ecdh_done, req);

	return -EINPROGRESS;

free_work_data:
	kfree(work_data);
	return ret;
}

static struct i2c_client *atmel_ecc_i2c_client_alloc(void)
{
	struct atmel_i2c_client_priv *i2c_priv, *min_i2c_priv = NULL;
	struct i2c_client *client = ERR_PTR(-ENODEV);
	int min_tfm_cnt = INT_MAX;
	int tfm_cnt;

	spin_lock(&driver_data.i2c_list_lock);

	if (list_empty(&driver_data.i2c_client_list)) {
		spin_unlock(&driver_data.i2c_list_lock);
		return ERR_PTR(-ENODEV);
	}

	list_for_each_entry(i2c_priv, &driver_data.i2c_client_list,
			    i2c_client_list_node) {
		tfm_cnt = atomic_read(&i2c_priv->tfm_count);
		if (tfm_cnt < min_tfm_cnt) {
			min_tfm_cnt = tfm_cnt;
			min_i2c_priv = i2c_priv;
		}
		if (!min_tfm_cnt)
			break;
	}

	if (min_i2c_priv) {
		atomic_inc(&min_i2c_priv->tfm_count);
		client = min_i2c_priv->client;
	}

	spin_unlock(&driver_data.i2c_list_lock);

	return client;
}

static void atmel_ecc_i2c_client_free(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);

	spin_lock(&driver_data.i2c_list_lock);
	if (atomic_dec_and_test(&i2c_priv->tfm_count) && i2c_priv->unbinding)
		complete(&i2c_priv->remove_done);
	spin_unlock(&driver_data.i2c_list_lock);
}

static int atmel_ecdh_init_tfm(struct crypto_kpp *tfm)
{
	const char *alg = kpp_alg_name(tfm);
	struct crypto_kpp *fallback;
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);

	ctx->client = atmel_ecc_i2c_client_alloc();
	if (IS_ERR(ctx->client)) {
		pr_err("tfm - i2c_client binding failed\n");
		return PTR_ERR(ctx->client);
	}

	fallback = crypto_alloc_kpp(alg, 0, CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(fallback)) {
		dev_err(&ctx->client->dev, "Failed to allocate transformation for '%s': %ld\n",
			alg, PTR_ERR(fallback));
		atmel_ecc_i2c_client_free(ctx->client);
		return PTR_ERR(fallback);
	}

	crypto_kpp_set_flags(fallback, crypto_kpp_get_flags(tfm));
	ctx->fallback = fallback;

	return 0;
}

static void atmel_ecdh_exit_tfm(struct crypto_kpp *tfm)
{
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);

	kfree(ctx->public_key);
	if (ctx->fallback)
		crypto_free_kpp(ctx->fallback);
	atmel_ecc_i2c_client_free(ctx->client);
}

static unsigned int atmel_ecdh_max_size(struct crypto_kpp *tfm)
{
	struct atmel_ecdh_ctx *ctx = kpp_tfm_ctx(tfm);

	return crypto_kpp_maxsize(ctx->fallback);
}

static int atmel_ecc_wait_for_tfms(struct atmel_i2c_client_priv *i2c_priv,
				   unsigned long timeout)
{
	spin_lock(&driver_data.i2c_list_lock);
	list_del(&i2c_priv->i2c_client_list_node);
	i2c_priv->unbinding = true;
	reinit_completion(&i2c_priv->remove_done);
	if (!atomic_read(&i2c_priv->tfm_count)) {
		spin_unlock(&driver_data.i2c_list_lock);
		return 0;
	}
	spin_unlock(&driver_data.i2c_list_lock);

	if (!wait_for_completion_timeout(&i2c_priv->remove_done,
					 msecs_to_jiffies(timeout)))
		return -ETIMEDOUT;

	return 0;
}

static struct kpp_alg atmel_ecdh_nist_p256 = {
	.set_secret = atmel_ecdh_set_secret,
	.generate_public_key = atmel_ecdh_generate_public_key,
	.compute_shared_secret = atmel_ecdh_compute_shared_secret,
	.init = atmel_ecdh_init_tfm,
	.exit = atmel_ecdh_exit_tfm,
	.max_size = atmel_ecdh_max_size,
	.base = {
		.cra_flags = CRYPTO_ALG_NEED_FALLBACK,
		.cra_name = "ecdh-nist-p256",
		.cra_driver_name = "atmel-ecdh",
		.cra_priority = ATMEL_ECC_PRIORITY,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct atmel_ecdh_ctx),
	},
};

static int atmel_ecc_probe(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv;
	unsigned long timeout;
	int ret;

	ret = atmel_i2c_probe(client);
	if (ret)
		return ret;

	i2c_priv = i2c_get_clientdata(client);

	init_completion(&i2c_priv->remove_done);
	i2c_priv->unbinding = false;

	spin_lock(&driver_data.i2c_list_lock);
	list_add_tail(&i2c_priv->i2c_client_list_node,
		      &driver_data.i2c_client_list);
	spin_unlock(&driver_data.i2c_list_lock);

	mutex_lock(&atmel_ecc_kpp_lock);
	/*
	 * For cases where the same/last such device is still in unregistering,
	 * and now re-registering (refcnt is 0, but completion still exists).
	 * Safely capture the pointer, drop the lock and sleep until it
	 * terminates upon completion or retry limit reached.
	 */
	while (atmel_ecc_unreg_active) {
		mutex_unlock(&atmel_ecc_kpp_lock);
		timeout = wait_for_completion_timeout(&atmel_ecc_unreg_done,
						      msecs_to_jiffies(2000));
		mutex_lock(&atmel_ecc_kpp_lock);
		if (timeout == 0) {
			mutex_unlock(&atmel_ecc_kpp_lock);
			/*
			 * FIXME / RFC: If we time out here, returning -ETIMEDOUT
			 * triggers devres cleanup, causing a UAF for any lagging TFMs.
			 * Should this be changed to an unbounded wait_for_completion()
			 * to prioritize memory safety over thread liveness?
			 */
			if (atmel_ecc_wait_for_tfms(i2c_priv, 2000))
				dev_emerg(&client->dev,
					  "Probe abort timed out! Active TFMs leaked, memory corruption imminent.\n");
			else
				dev_err(&client->dev,
					"Probe timed out waiting for former instance unregistration\n");

			return -ETIMEDOUT;
		}
	}
	if (atmel_ecc_kpp_refcnt == 0) {
		ret = crypto_register_kpp(&atmel_ecdh_nist_p256);
		if (ret) {
			mutex_unlock(&atmel_ecc_kpp_lock);

			atmel_ecc_wait_for_tfms(i2c_priv, 2000);
			dev_err(&client->dev,
				"%s alg registration failed\n",
				atmel_ecdh_nist_p256.base.cra_driver_name);

			return ret;
		}
	}
	atmel_ecc_kpp_refcnt++;
	mutex_unlock(&atmel_ecc_kpp_lock);

	dev_info(&client->dev, "atmel ecc algorithms registered in /proc/crypto\n");
	return ret;
}

static void atmel_ecc_remove(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);
	bool trigger_unreg = false;

	/*
	 * FIXME / RFC: The timeout prevents a permanent hang,
	 * but since remove() returns void, devres will instantly
	 * free i2c_priv anyway. Memory corruption is imminent
	 * when the active TFM eventually closes.
	 */
	if (atmel_ecc_wait_for_tfms(i2c_priv, 5000))
		dev_emerg(&client->dev,
			  "Teardown timed out! Active TFMs leak, memory corruption imminent.\n");

	mutex_lock(&atmel_ecc_kpp_lock);
	atmel_ecc_kpp_refcnt--;
	if (atmel_ecc_kpp_refcnt == 0) {
		trigger_unreg = true;
		atmel_ecc_unreg_active = true;
		reinit_completion(&atmel_ecc_unreg_done);
	}
	mutex_unlock(&atmel_ecc_kpp_lock);

	if (trigger_unreg) {
		crypto_unregister_kpp(&atmel_ecdh_nist_p256);
		mutex_lock(&atmel_ecc_kpp_lock);
		atmel_ecc_unreg_active = false;
		complete_all(&atmel_ecc_unreg_done);
		mutex_unlock(&atmel_ecc_kpp_lock);
	}
}

static const struct of_device_id atmel_ecc_dt_ids[] = {
	{ .compatible = "atmel,atecc508a", },
	{ .compatible = "atmel,atecc608b", },
	{ }
};
MODULE_DEVICE_TABLE(of, atmel_ecc_dt_ids);

static const struct i2c_device_id atmel_ecc_id[] = {
	{ .name = "atecc508a" },
	{ .name = "atecc608b" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, atmel_ecc_id);

static struct i2c_driver atmel_ecc_driver = {
	.driver = {
		.name	= "atmel-ecc",
		.of_match_table = atmel_ecc_dt_ids,
	},
	.probe		= atmel_ecc_probe,
	.remove		= atmel_ecc_remove,
	.id_table	= atmel_ecc_id,
};

static int __init atmel_ecc_init(void)
{
	spin_lock_init(&driver_data.i2c_list_lock);
	INIT_LIST_HEAD(&driver_data.i2c_client_list);
	return i2c_add_driver(&atmel_ecc_driver);
}

static void __exit atmel_ecc_exit(void)
{
	atmel_i2c_flush_queue();
	i2c_del_driver(&atmel_ecc_driver);
}

module_init(atmel_ecc_init);
module_exit(atmel_ecc_exit);

MODULE_AUTHOR("Tudor Ambarus");
MODULE_DESCRIPTION("Microchip / Atmel ECC (I2C) driver");
MODULE_LICENSE("GPL v2");
