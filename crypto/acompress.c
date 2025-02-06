// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Asynchronous Compression operations
 *
 * Copyright (c) 2016, Intel Corporation
 * Authors: Weigang Li <weigang.li@intel.com>
 *          Giovanni Cabiddu <giovanni.cabiddu@intel.com>
 */

#include <crypto/internal/acompress.h>
#include <linux/cryptouser.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/netlink.h>

#include "compress.h"

struct crypto_scomp;

static const struct crypto_type crypto_acomp_type;

struct acomp_save_req_state {
	struct list_head head;
	struct acomp_req *req0;
	struct acomp_req *cur;
	int (*op)(struct acomp_req *req);
	crypto_completion_t compl;
	void *data;
};

static void acomp_reqchain_done(void *data, int err);
static int acomp_save_req(struct acomp_req *req, crypto_completion_t cplt);
static void acomp_restore_req(struct acomp_req *req);

static inline struct acomp_alg *__crypto_acomp_alg(struct crypto_alg *alg)
{
	return container_of(alg, struct acomp_alg, calg.base);
}

static inline struct acomp_alg *crypto_acomp_alg(struct crypto_acomp *tfm)
{
	return __crypto_acomp_alg(crypto_acomp_tfm(tfm)->__crt_alg);
}

static int __maybe_unused crypto_acomp_report(
	struct sk_buff *skb, struct crypto_alg *alg)
{
	struct crypto_report_acomp racomp;

	memset(&racomp, 0, sizeof(racomp));

	strscpy(racomp.type, "acomp", sizeof(racomp.type));

	return nla_put(skb, CRYPTOCFGA_REPORT_ACOMP, sizeof(racomp), &racomp);
}

static void crypto_acomp_show(struct seq_file *m, struct crypto_alg *alg)
	__maybe_unused;

static void crypto_acomp_show(struct seq_file *m, struct crypto_alg *alg)
{
	seq_puts(m, "type         : acomp\n");
}

static void crypto_acomp_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_acomp *acomp = __crypto_acomp_tfm(tfm);
	struct acomp_alg *alg = crypto_acomp_alg(acomp);

	alg->exit(acomp);
}

static int crypto_acomp_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_acomp *acomp = __crypto_acomp_tfm(tfm);
	struct acomp_alg *alg = crypto_acomp_alg(acomp);

	if (tfm->__crt_alg->cra_type != &crypto_acomp_type)
		return crypto_init_scomp_ops_async(tfm);

	acomp->compress = alg->compress;
	acomp->decompress = alg->decompress;
	acomp->get_batch_size = alg->get_batch_size;
	acomp->batch_compress = alg->batch_compress;
	acomp->batch_decompress = alg->batch_decompress;
	acomp->dst_free = alg->dst_free;
	acomp->reqsize = alg->reqsize;

	if (alg->exit)
		acomp->base.exit = crypto_acomp_exit_tfm;

	if (alg->init)
		return alg->init(acomp);

	return 0;
}

static unsigned int crypto_acomp_extsize(struct crypto_alg *alg)
{
	int extsize = crypto_alg_extsize(alg);

	if (alg->cra_type != &crypto_acomp_type)
		extsize += sizeof(struct crypto_scomp *);

	return extsize;
}

static const struct crypto_type crypto_acomp_type = {
	.extsize = crypto_acomp_extsize,
	.init_tfm = crypto_acomp_init_tfm,
#ifdef CONFIG_PROC_FS
	.show = crypto_acomp_show,
#endif
#if IS_ENABLED(CONFIG_CRYPTO_USER)
	.report = crypto_acomp_report,
#endif
	.maskclear = ~CRYPTO_ALG_TYPE_MASK,
	.maskset = CRYPTO_ALG_TYPE_ACOMPRESS_MASK,
	.type = CRYPTO_ALG_TYPE_ACOMPRESS,
	.tfmsize = offsetof(struct crypto_acomp, base),
};

struct crypto_acomp *crypto_alloc_acomp(const char *alg_name, u32 type,
					u32 mask)
{
	return crypto_alloc_tfm(alg_name, &crypto_acomp_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_alloc_acomp);

struct crypto_acomp *crypto_alloc_acomp_node(const char *alg_name, u32 type,
					u32 mask, int node)
{
	return crypto_alloc_tfm_node(alg_name, &crypto_acomp_type, type, mask,
				node);
}
EXPORT_SYMBOL_GPL(crypto_alloc_acomp_node);

static int acomp_save_req(struct acomp_req *req, crypto_completion_t cplt)
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);
	struct acomp_save_req_state *state;
	gfp_t gfp;
	u32 flags;

	if (!acomp_is_async(tfm))
		return 0;

	flags = acomp_request_flags(req);
	gfp = (flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?  GFP_KERNEL : GFP_ATOMIC;
	state = kmalloc(sizeof(*state), gfp);
	if (!state)
		return -ENOMEM;

	state->compl = req->base.complete;
	state->data = req->base.data;
	state->req0 = req;

	req->base.complete = cplt;
	req->base.data = state;

	return 0;
}

static void acomp_restore_req(struct acomp_req *req)
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);
	struct acomp_save_req_state *state;

	if (!acomp_is_async(tfm))
		return;

	state = req->base.data;

	req->base.complete = state->compl;
	req->base.data = state->data;
	kfree(state);
}

static int acomp_reqchain_finish(struct acomp_save_req_state *state,
				 int err, u32 mask)
{
	struct acomp_req *req0 = state->req0;
	struct acomp_req *req = state->cur;
	struct acomp_req *n;

	req->base.err = err;

	if (req == req0)
		INIT_LIST_HEAD(&req->base.list);
	else
		list_add_tail(&req->base.list, &req0->base.list);

	list_for_each_entry_safe(req, n, &state->head, base.list) {
		list_del_init(&req->base.list);

		req->base.flags &= mask;
		req->base.complete = acomp_reqchain_done;
		req->base.data = state;
		state->cur = req;
		err = state->op(req);

		if (err == -EINPROGRESS) {
			if (!list_empty(&state->head))
				err = -EBUSY;
			goto out;
		}

		if (err == -EBUSY)
			goto out;

		req->base.err = err;
		list_add_tail(&req->base.list, &req0->base.list);
	}

	acomp_restore_req(req0);

out:
	return err;
}

static void acomp_reqchain_done(void *data, int err)
{
	struct acomp_save_req_state *state = data;
	crypto_completion_t compl = state->compl;

	data = state->data;

	if (err == -EINPROGRESS) {
		if (!list_empty(&state->head))
			return;
		goto notify;
	}

	err = acomp_reqchain_finish(state, err, CRYPTO_TFM_REQ_MAY_BACKLOG);
	if (err == -EBUSY)
		return;

notify:
	compl(data, err);
}

int acomp_do_req_chain(struct acomp_req *req,
		       int (*op)(struct acomp_req *req))
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);
	struct acomp_save_req_state *state;
	struct acomp_save_req_state state0;
	int err = 0;

	if (!acomp_request_chained(req) || list_empty(&req->base.list) ||
	    !crypto_acomp_req_chain(tfm))
		return op(req);

	state = &state0;

	if (acomp_is_async(tfm)) {
		err = acomp_save_req(req, acomp_reqchain_done);
		if (err) {
			struct acomp_req *r2;

			req->base.err = err;
			list_for_each_entry(r2, &req->base.list, base.list)
				r2->base.err = err;

			return err;
		}

		state = req->base.data;
	}

	state->op = op;
	state->cur = req;
	INIT_LIST_HEAD(&state->head);
	list_splice(&req->base.list, &state->head);

	err = op(req);
	if (err == -EBUSY || err == -EINPROGRESS)
		return -EBUSY;

	return acomp_reqchain_finish(state, err, ~0);
}
EXPORT_SYMBOL_GPL(acomp_do_req_chain);

static void acomp_async_reqchain_done(struct acomp_req *req0,
				      struct list_head *state,
				      int (*op_poll)(struct acomp_req *req))
{
	struct acomp_req *req, *n;
	bool req0_done = false;
	int err;

	while (!list_empty(state)) {

		if (!req0_done) {
			err = op_poll(req0);
			if (!(err == -EAGAIN || err == -EINPROGRESS || err == -EBUSY)) {
				req0->base.err = err;
				req0_done = true;
			}
		}

		list_for_each_entry_safe(req, n, state, base.list) {
			err = op_poll(req);

			if (err == -EAGAIN || err == -EINPROGRESS || err == -EBUSY)
				continue;

			req->base.err = err;
			list_del_init(&req->base.list);
			list_add_tail(&req->base.list, &req0->base.list);
		}
	}

	while (!req0_done) {
		err = op_poll(req0);
		if (!(err == -EAGAIN || err == -EINPROGRESS || err == -EBUSY)) {
			req0->base.err = err;
			break;
		}
	}
}

static int acomp_async_reqchain_finish(struct acomp_req *req0,
				       struct list_head *state,
				       int (*op_submit)(struct acomp_req *req),
				       int (*op_poll)(struct acomp_req *req))
{
	struct acomp_req *req, *n;
	int err = 0;

	INIT_LIST_HEAD(&req0->base.list);

	list_for_each_entry_safe(req, n, state, base.list) {
		BUG_ON(req == req0);

		err = op_submit(req);

		if (!(err == -EINPROGRESS || err == -EBUSY)) {
			req->base.err = err;
			list_del_init(&req->base.list);
			list_add_tail(&req->base.list, &req0->base.list);
		}
	}

	acomp_async_reqchain_done(req0, state, op_poll);

	return req0->base.err;
}

int acomp_do_async_req_chain(struct acomp_req *req,
			     int (*op_submit)(struct acomp_req *req),
			     int (*op_poll)(struct acomp_req *req))
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);
	struct list_head state;
	struct acomp_req *r2;
	int err = 0;
	void *req0_data = req->base.data;

	if (!acomp_request_chained(req) || list_empty(&req->base.list) ||
		!acomp_is_async(tfm) || !crypto_acomp_req_chain(tfm)) {

		err = op_submit(req);

		if (err == -EINPROGRESS || err == -EBUSY) {
			bool req0_done = false;

			while (!req0_done) {
				err = op_poll(req);
				if (!(err == -EAGAIN || err == -EINPROGRESS || err == -EBUSY)) {
					req->base.err = err;
					break;
				}
			}
		} else {
			req->base.err = err;
		}

		req->base.data = req0_data;
		if (acomp_is_async(tfm))
			req->base.complete(req->base.data, req->base.err);

		return err;
	}

	err = op_submit(req);
	req->base.err = err;

	if (err && !(err == -EINPROGRESS || err == -EBUSY))
		goto err_prop;

	INIT_LIST_HEAD(&state);
	list_splice(&req->base.list, &state);

	err = acomp_async_reqchain_finish(req, &state, op_submit, op_poll);
	req->base.data = req0_data;
	req->base.complete(req->base.data, req->base.err);

	return err;

err_prop:
	list_for_each_entry(r2, &req->base.list, base.list)
		r2->base.err = err;

	return err;
}
EXPORT_SYMBOL_GPL(acomp_do_async_req_chain);

struct acomp_req *acomp_request_alloc(struct crypto_acomp *acomp)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp);
	struct acomp_req *req;

	req = __acomp_request_alloc(acomp);
	if (req && (tfm->__crt_alg->cra_type != &crypto_acomp_type))
		return crypto_acomp_scomp_alloc_ctx(req);

	return req;
}
EXPORT_SYMBOL_GPL(acomp_request_alloc);

void acomp_request_free(struct acomp_req *req)
{
	struct crypto_acomp *acomp = crypto_acomp_reqtfm(req);
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp);

	if (tfm->__crt_alg->cra_type != &crypto_acomp_type)
		crypto_acomp_scomp_free_ctx(req);

	if (req->flags & CRYPTO_ACOMP_ALLOC_OUTPUT) {
		acomp->dst_free(req->dst);
		req->dst = NULL;
	}

	__acomp_request_free(req);
}
EXPORT_SYMBOL_GPL(acomp_request_free);

void comp_prepare_alg(struct comp_alg_common *alg)
{
	struct crypto_alg *base = &alg->base;

	base->cra_flags &= ~CRYPTO_ALG_TYPE_MASK;
}

int crypto_register_acomp(struct acomp_alg *alg)
{
	struct crypto_alg *base = &alg->calg.base;

	comp_prepare_alg(&alg->calg);

	base->cra_type = &crypto_acomp_type;
	base->cra_flags |= CRYPTO_ALG_TYPE_ACOMPRESS;

	return crypto_register_alg(base);
}
EXPORT_SYMBOL_GPL(crypto_register_acomp);

void crypto_unregister_acomp(struct acomp_alg *alg)
{
	crypto_unregister_alg(&alg->base);
}
EXPORT_SYMBOL_GPL(crypto_unregister_acomp);

int crypto_register_acomps(struct acomp_alg *algs, int count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = crypto_register_acomp(&algs[i]);
		if (ret)
			goto err;
	}

	return 0;

err:
	for (--i; i >= 0; --i)
		crypto_unregister_acomp(&algs[i]);

	return ret;
}
EXPORT_SYMBOL_GPL(crypto_register_acomps);

void crypto_unregister_acomps(struct acomp_alg *algs, int count)
{
	int i;

	for (i = count - 1; i >= 0; --i)
		crypto_unregister_acomp(&algs[i]);
}
EXPORT_SYMBOL_GPL(crypto_unregister_acomps);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Asynchronous compression type");
