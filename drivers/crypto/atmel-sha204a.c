// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip / Atmel SHA204A (I2C) driver.
 *
 * Copyright (c) 2019 Linaro, Ltd. <ard.biesheuvel@linaro.org>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <crypto/sha2.h>

#include "atmel-i2c.h"

static int atmel_sha204a_sha_init_tfm(struct crypto_tfm *tfm)
{
	struct atmel_i2c_sha_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->client = atmel_i2c_client_alloc(ATMEL_CAP_SHA);
	if (IS_ERR(ctx->client)) {
		pr_err("tfm - i2c_client binding failed\n");
		return PTR_ERR(ctx->client);
	}

	return 0;
}

static struct ahash_alg atmel_sha204a_sha = {
	.init = atmel_i2c_sha_init,
	.update	= atmel_i2c_sha_update,
	.final = atmel_i2c_sha_final,
	.finup = atmel_i2c_sha_finup,
	.digest	= atmel_i2c_sha_digest,
	.export = atmel_i2c_sha_export,
	.import = atmel_i2c_sha_import,
	.halg = {
		.digestsize = SHA256_DIGEST_SIZE,
		.statesize = sizeof(struct atmel_i2c_sha_reqctx),
		.base = {
			.cra_name		= "sha256",
			.cra_driver_name	= "atmel-sha256",
			.cra_init		= atmel_sha204a_sha_init_tfm,
			.cra_priority		= ATMEL_I2C_PRIORITY,
			.cra_flags		= CRYPTO_ALG_TYPE_AHASH,
			.cra_blocksize		= SHA256_BLOCK_SIZE,
			.cra_ctxsize		= sizeof(struct atmel_i2c_sha_ctx),
			.cra_reqsize		= sizeof(struct atmel_i2c_sha_reqctx),
			.cra_module		= THIS_MODULE,
		}
	}
};

static ssize_t config_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return atmel_i2c_eeprom_display(dev, attr, buf, ATMEL_EEPROM_CONFIG_ZONE);
}
static DEVICE_ATTR_ADMIN_RO(config);

static ssize_t otp_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return atmel_i2c_eeprom_display(dev, attr, buf, ATMEL_EEPROM_OTP_ZONE);
}
static DEVICE_ATTR_RO(otp);

static struct attribute *atmel_sha204a_attrs[] = {
	&dev_attr_config.attr,
	&dev_attr_otp.attr,
	NULL
};

static const struct attribute_group atmel_sha204a_groups = {
	.name = "atsha204a",
	.attrs = atmel_sha204a_attrs,
};

static int atmel_sha204a_probe(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv;
	const struct atmel_i2c_of_match_data *data;
	int ret;

	ret = atmel_i2c_probe(client);
	if (ret)
		goto done;

	data = device_get_match_data(&client->dev);
	if (!data) {
		dev_err(&client->dev, "no match data found via OF or ID table\n");
		ret = -ENODEV;
		goto done;
	}

	i2c_priv = i2c_get_clientdata(client);
	i2c_priv->data = data;
	i2c_priv->caps = BIT(ATMEL_CAP_SHA);

	ret = atmel_i2c_device_sanity_check(client);
	if (ret) {
		dev_err(&client->dev, "failed to read EEPROM, is hardware attached?\n");
		goto done;
	}

	/* add to client list */
	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);
	list_add_tail(&i2c_priv->i2c_client_list_node,
		      &atmel_i2c_mgmt.i2c_client_list);
	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);

	/* EEPROM read out */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		ret = -ENODEV;
		goto err_list_del;
	}

	ret = sysfs_create_group(&client->dev.kobj, &atmel_sha204a_groups);
	if (ret) {
		dev_err(&client->dev, "failed to register sysfs entry\n");
		goto err_list_del;
	}

	/* register rng */
	ret = atmel_i2c_register_rng(i2c_priv, &client->dev);
	if (ret) {
		dev_err(&client->dev, "failed to register hw_random\n");
		goto err_list_del;
	}

	/* register algorithms */
	ret = crypto_register_ahash(&atmel_sha204a_sha);
	if (ret) {
		dev_err(&client->dev, "SHA256 registration failed\n");
		goto err_list_del;
	}

	goto done;

err_list_del:
	sysfs_remove_group(&client->dev.kobj, &atmel_sha204a_groups);
	spin_lock(&atmel_i2c_mgmt.i2c_list_lock);
	list_del(&i2c_priv->i2c_client_list_node);
	spin_unlock(&atmel_i2c_mgmt.i2c_list_lock);

done:
	return ret;
}

static void atmel_sha204a_remove(struct i2c_client *client)
{
	struct atmel_i2c_client_priv *i2c_priv = i2c_get_clientdata(client);

	sysfs_remove_group(&client->dev.kobj, &atmel_sha204a_groups);

	if (!i2c_priv)
		return;

	devm_hwrng_unregister(&client->dev, &i2c_priv->hwrng);
	atmel_i2c_flush_queue();

	crypto_unregister_ahash(&atmel_sha204a_sha);

	if (i2c_priv->hwrng.priv) {
		kfree((void *)i2c_priv->hwrng.priv);
		i2c_priv->hwrng.priv = 0;
	}
}

static const struct atmel_i2c_of_match_data atsha204_match_data = {
	.timings = {
		.max_exec_time_genkey = 43,
		.max_exec_time_random = 50,
		.max_exec_time_read = 4,
		.max_exec_time_sha = 22,
		.max_exec_time_write = 42,
	},
	.eeprom_zone_size = {
		[ATMEL_EEPROM_CONFIG_ZONE] = 88,
		[ATMEL_EEPROM_OTP_ZONE] = 64,
		[ATMEL_EEPROM_DATA_ZONE] = 512
	},
	/*
	 * According to review by Bill Cox [1], the ATSHA204 has very low entropy.
	 * [1] https://www.metzdowd.com/pipermail/cryptography/2014-December/023858.html
	 */
	.needs_legacy_hwrng = 1,
	.needs_sha_padding = 1,
};

static const struct atmel_i2c_of_match_data atsha204a_match_data = {
	.timings = {
		.max_exec_time_genkey = 43,
		.max_exec_time_random = 50,
		.max_exec_time_read = 4,
		.max_exec_time_sha = 22,
		.max_exec_time_write = 42,
	},
	.eeprom_zone_size = {
		[ATMEL_EEPROM_CONFIG_ZONE] = 88,
		[ATMEL_EEPROM_OTP_ZONE] = 64,
		[ATMEL_EEPROM_DATA_ZONE] = 512
	},
	.needs_sha_padding = 1,
};

static const struct of_device_id atmel_sha204a_dt_ids[] __maybe_unused = {
	{ .compatible = "atmel,atsha204", .data = &atsha204_match_data, },
	{ .compatible = "atmel,atsha204a", .data = &atsha204a_match_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atmel_sha204a_dt_ids);

static const struct i2c_device_id atmel_sha204a_id[] = {
	{ "atsha204" },
	{ "atsha204a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, atmel_sha204a_id);

static struct i2c_driver atmel_sha204a_driver = {
	.probe			= atmel_sha204a_probe,
	.remove			= atmel_sha204a_remove,
	.id_table		= atmel_sha204a_id,

	.driver.name		= "atmel-sha204a",
	.driver.of_match_table	= of_match_ptr(atmel_sha204a_dt_ids),
};

module_i2c_driver(atmel_sha204a_driver);

MODULE_AUTHOR("Ard Biesheuvel <ard.biesheuvel@linaro.org>");
MODULE_DESCRIPTION("Microchip / Atmel SHA204A (I2C) driver");
MODULE_LICENSE("GPL v2");
