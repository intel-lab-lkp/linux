// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tee_drv.h>
#include <linux/types.h>

#include "mbedtee_drv.h"
#include "shm_pool.h"

int mbedtee_get_resource(struct device_node *node,
			 const char *name, struct resource *res)
{
	struct device_node *rmem;
	int idx;
	int ret;

	idx = of_property_match_string(node, "memory-region-names", name);
	if (idx < 0)
		return idx;

	rmem = of_parse_phandle(node, "memory-region", idx);
	if (!rmem)
		return -ENODEV;

	ret = of_address_to_resource(rmem, 0, res);
	of_node_put(rmem);

	if (ret)
		return ret;

	res->name = name;
	return 0;
}

static void mbedtee_get_version(struct tee_device *teedev,
				struct tee_ioctl_version_data *vers)
{
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_MBEDTEE,
		.impl_caps = 0,
		.gen_caps = TEE_GEN_CAP_GP | TEE_GEN_CAP_REG_MEM |
			    TEE_GEN_CAP_MEMREF_NULL,
	};

	*vers = v;
}

static int mbedtee_open(struct tee_context *ctx)
{
	struct mbedtee_context_data *ctxdata;

	ctxdata = kzalloc_obj(*ctxdata, GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	mutex_init(&ctxdata->mutex);
	INIT_LIST_HEAD(&ctxdata->sess_list);

	ctx->data = ctxdata;
	ctx->cap_memref_null = true;

	return 0;
}

static void mbedtee_release(struct tee_context *ctx)
{
	struct mbedtee_device *mbedtee = tee_get_drvdata(ctx->teedev);
	struct mbedtee_context_data *d = ctx->data;
	struct mbedtee_session *s, *n;

	if (!d)
		return;

	if (d->is_supp_ctx) {
		mbedtee_supp_release(&mbedtee->supp, ctx);
	} else {
		list_for_each_entry_safe(s, n, &d->sess_list, list_node)
			mbedtee_close_session(ctx, s->session_id);
	}

	kfree(d);
	ctx->data = NULL;
}

static const struct tee_driver_ops mbedtee_ops = {
	.get_version	= mbedtee_get_version,
	.open		= mbedtee_open,
	.release	= mbedtee_release,
	.open_session	= mbedtee_open_session,
	.close_session	= mbedtee_close_session,
	.invoke_func	= mbedtee_invoke_func,
	.cancel_req	= mbedtee_cancel_req,
	.supp_recv	= mbedtee_supp_recv,
	.supp_send	= mbedtee_supp_send,
	.shm_register	= mbedtee_shm_register,
	.shm_unregister	= mbedtee_shm_unregister,
};

static const struct tee_desc mbedtee_desc = {
	.name	= "mbedtee",
	.ops	= &mbedtee_ops,
	.owner	= THIS_MODULE,
};

static int mbedtee_probe(struct platform_device *pdev)
{
	int ret;
	long version;
	long yield;
	struct tee_shm_pool *pool;
	struct tee_device *teedev;
	struct mbedtee_device *mbedtee;

	mbedtee = devm_kzalloc(&pdev->dev, sizeof(*mbedtee), GFP_KERNEL);
	if (!mbedtee)
		return -ENOMEM;

	mbedtee->dev = &pdev->dev;
	xa_init_flags(&mbedtee->rpc_calls, XA_FLAGS_ALLOC1);
	mbedtee_supp_init(&mbedtee->supp);

	/*
	 * R2T must be ready before T2R is advertised: synchronous T2R RPC
	 * work replies with COMPLETE_TEE over the R2T path.
	 */
	ret = mbedtee_r2t_init(mbedtee);
	if (ret != 0)
		goto err_supp;

	ret = mbedtee_rpc_init(mbedtee);
	if (ret != 0)
		goto err_r2t;

	version = mbedtee_rpc_fastcall(mbedtee, MBEDTEE_RPC_OS_VERSION, 0, 0, 0);
	if (version < 0) {
		dev_err(&pdev->dev, "MBEDTEE_RPC_OS_VERSION failed: %ld\n", version);
		ret = version;
		goto err_rpc;
	}

	if (!MBEDTEE_VALID_VERSION(version)) {
		dev_err(&pdev->dev, "mbedtee not present (version=0x%lx)\n",
			version);
		ret = -ENODEV;
		goto err_rpc;
	}

	yield = mbedtee_rpc_fastcall(mbedtee, MBEDTEE_RPC_SUPPORT_YIELD, 0, 0, 0);
	dev_info(&pdev->dev, "version: 0x%06lx  yield: %ld\n", version, yield);

	pool = mbedtee_shm_pool_alloc_pages();
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto err_rpc;
	}

	teedev = tee_device_alloc(&mbedtee_desc, &pdev->dev, pool, mbedtee);
	if (IS_ERR(teedev)) {
		ret = PTR_ERR(teedev);
		goto err_pool;
	}

	ret = tee_device_register(teedev);
	if (ret)
		goto err_teedev;

	mbedtee->teedev = teedev;
	mbedtee->version = version;
	mbedtee->yield = !!yield;
	mbedtee->pool = pool;
	platform_set_drvdata(pdev, mbedtee);

	dev_dbg(&pdev->dev, "client initialized\n");
	return 0;

err_teedev:
	tee_device_unregister(teedev);
err_pool:
	tee_shm_pool_free(pool);
err_rpc:
	mbedtee_rpc_uninit(mbedtee);
err_r2t:
	mbedtee_r2t_uninit(mbedtee);
err_supp:
	mbedtee_supp_uninit(&mbedtee->supp);
	xa_destroy(&mbedtee->rpc_calls);
	return ret;
}

static void mbedtee_remove(struct platform_device *pdev)
{
	struct mbedtee_device *mbedtee = platform_get_drvdata(pdev);

	tee_device_unregister(mbedtee->teedev);
	/*
	 * Drain T2R before tearing down R2T: queued RPC work may still need
	 * to send COMPLETE_TEE over R2T. Destroy the supplicant mutex only
	 * after RPC work can no longer enter mbedtee_supp_handler().
	 */
	mbedtee_rpc_uninit(mbedtee);
	mbedtee_r2t_uninit(mbedtee);
	mbedtee_supp_uninit(&mbedtee->supp);
	tee_shm_pool_free(mbedtee->pool);
	xa_destroy(&mbedtee->rpc_calls);
}

static const struct of_device_id mbedtee_dt_match[] = {
	{ .compatible = "mbedtee,rpc" },
	{ },
};
MODULE_DEVICE_TABLE(of, mbedtee_dt_match);

static struct platform_driver mbedtee_driver = {
	.probe	= mbedtee_probe,
	.remove	= mbedtee_remove,
	.driver = {
		.name		= "mbedtee",
		.of_match_table	= mbedtee_dt_match,
	},
};
module_platform_driver(mbedtee_driver);

MODULE_AUTHOR("Xing Loong <xing.xl.loong@gmail.com>");
MODULE_DESCRIPTION("MbedTEE Trusted Execution Environment driver");
MODULE_LICENSE("GPL");
