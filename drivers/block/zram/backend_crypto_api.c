// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/crypto.h>

#include "backend_crypto_api.h"

struct crypto_api_ctx {
	struct crypto_comp *tfm;
};

extern struct list_head crypto_alg_list;

static void crypto_api_release_params(struct zcomp_params *params)
{
}

static int crypto_api_setup_params(struct zcomp_params *params)
{
	return 0;
}

static int crypto_api_create(struct zcomp *zcomp, struct zcomp_ctx *ctx)
{
	struct crypto_api_ctx *crypto_ctx;
	const char *algname = zcomp->ops->name;

	crypto_ctx = kzalloc(sizeof(*crypto_ctx), GFP_KERNEL);
	if (!crypto_ctx)
		return -ENOMEM;

	crypto_ctx->tfm = crypto_alloc_comp(algname, 0, 0);
	if (IS_ERR_OR_NULL(crypto_ctx->tfm)) {
		kfree(crypto_ctx);
		return -ENOMEM;
	}

	ctx->context = crypto_ctx;

	return 0;
}

static void crypto_api_destroy(struct zcomp_ctx *ctx)
{
	struct crypto_api_ctx *crypto_ctx = ctx->context;

	if (!IS_ERR_OR_NULL(crypto_ctx->tfm))
		crypto_free_comp(crypto_ctx->tfm);

	kfree(crypto_ctx);
}

static int crypto_api_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			       struct zcomp_req *req)
{
	struct crypto_api_ctx *crypto_ctx = ctx->context;
	unsigned int dst_len = req->dst_len;
	int ret;

	ret = crypto_comp_compress(crypto_ctx->tfm,
				   req->src, req->src_len,
				   req->dst, &dst_len);

	req->dst_len = dst_len;

	return ret;
}

static int crypto_api_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
				 struct zcomp_req *req)
{
	struct crypto_api_ctx *crypto_ctx = ctx->context;
	unsigned int dst_len = req->dst_len;
	int ret;

	ret = crypto_comp_decompress(crypto_ctx->tfm,
				     req->src, req->src_len,
				     req->dst, &dst_len);

	req->dst_len = dst_len;

	return ret;
}

static void crypto_api_destroy_ops(struct zcomp_ops *ops)
{
	kfree(ops->name);
	kfree(ops);
}

struct zcomp_ops *get_backend_crypto_api(const char *name)
{
	struct zcomp_ops *ops;
	char *algname;

	ops = kmalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	algname = kstrdup(name, GFP_KERNEL);
	if (!algname) {
		kfree(ops);
		return ERR_PTR(-ENOMEM);
	}

	ops->compress = crypto_api_compress;
	ops->decompress = crypto_api_decompress,
	ops->create_ctx = crypto_api_create,
	ops->destroy_ctx = crypto_api_destroy,
	ops->setup_params = crypto_api_setup_params,
	ops->release_params = crypto_api_release_params,
	ops->destroy = crypto_api_destroy_ops,
	ops->name = algname;

	return ops;
}
