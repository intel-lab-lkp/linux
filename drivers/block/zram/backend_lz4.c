#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/kfifo.h>
#include <linux/completion.h>

#include "backend_lz4.h"
#include "zcomp.h"

struct lz4_ctx {
	void *mem;

	LZ4_streamDecode_t *dstrm;
	LZ4_stream_t *cstrm;
};

struct lz4_stream {
	struct managed_zstrm zstrm;
	struct list_head node;
	struct completion completion;
	int result;
};

struct lz4_req {
	struct lz4_stream *strm;
	struct lz4_ctx *ctx;
	struct zcomp_params *params;
	struct zcomp_req *zreq;
};

#define BACKEND_LZ4_STREAM_MAX 128
#define BACKEND_LZ4_QUEUE_NR 2

struct lz4_global {
	struct list_head stream_head;
	struct mutex stream_lock;
	struct task_struct *tsk;

	struct completion new_task_ready;

	struct mutex working_lock;
	DECLARE_KFIFO(working_queue[BACKEND_LZ4_QUEUE_NR], struct lz4_req,
		      BACKEND_LZ4_STREAM_MAX);
	int working_submit_idx;

	struct kref ref;
};

static DEFINE_MUTEX(lz4_global_lock);
static struct lz4_global *lz4_global_data;

static void lz4_stream_free(struct lz4_stream *src)
{
	if (IS_ERR_OR_NULL(src))
		return;

	vfree(src->zstrm.strm.buffer);
	vfree(src->zstrm.strm.local_copy);

	kvfree(src);
}

DEFINE_FREE(lz4_stream, struct lz4_stream *, lz4_stream_free(_T));
static struct lz4_stream *lz4_stream_alloc(void)
{
	struct lz4_stream *strm __free(lz4_stream) = NULL;
	void *buffer __free(kvfree) = NULL;
	void *local_copy __free(kvfree) = NULL;

	strm = kvzalloc_obj(struct lz4_stream, GFP_KERNEL);
	if (!strm)
		return ERR_PTR(-ENOMEM);

	buffer = vmalloc(PAGE_SIZE * 2);
	local_copy = vmalloc(PAGE_SIZE);
	if (!buffer || !local_copy)
		return ERR_PTR(-ENOMEM);

	strm->zstrm.strm.buffer = no_free_ptr(buffer);
	strm->zstrm.strm.local_copy = no_free_ptr(local_copy);
	strm->zstrm.strm.zcomp_managed = true;

	return_ptr(strm);
}

static void lz4_streams_destroy(struct lz4_global *inst)
{
	struct lz4_stream *pos, *tmp;

	list_for_each_entry_safe(pos, tmp, &inst->stream_head, node) {
		list_del(&pos->node);
		lz4_stream_free(pos);
	}
}

static int lz4_streams_init(struct zcomp_params *params,
			    struct lz4_global *inst)
{
	int err = 0;

	INIT_LIST_HEAD(&inst->stream_head);
	for (int i = 0; i < BACKEND_LZ4_STREAM_MAX; i++) {
		struct lz4_stream *curr_zstrm __free(lz4_stream) = NULL;

		curr_zstrm = lz4_stream_alloc();
		if (IS_ERR(curr_zstrm)) {
			err = PTR_ERR(curr_zstrm);
			break;
		}

		/* lz4_ctx is linked to stream in get_stream() */
		list_add(&curr_zstrm->node, &inst->stream_head);
		init_completion(&curr_zstrm->completion);
		retain_and_null_ptr(curr_zstrm);
	}

	if (err) {
		lz4_streams_destroy(inst);
		return err;
	}

	return 0;
}

static int __lz4_compress(struct zcomp_params *params, struct lz4_ctx *zctx,
			  struct zcomp_req *req);

static void lz4_do_compression(struct lz4_global *inst)
{
	struct lz4_req req;
	int idx;

	scoped_guard(mutex, &inst->working_lock)
	{
		idx = inst->working_submit_idx;
		inst->working_submit_idx = (idx + 1) % BACKEND_LZ4_QUEUE_NR;
	}

	while (kfifo_get(&inst->working_queue[idx], &req)) {
		struct lz4_stream *lz4_strm = req.strm;

		lz4_strm->result =
			__lz4_compress(req.params, req.ctx, req.zreq);
		complete(&lz4_strm->completion);
	}
}

static int lz4_thread_worker(void *data)
{
	struct lz4_global *inst = data;

	while (!kthread_should_stop()) {
		int err;

		err = wait_for_completion_interruptible(&inst->new_task_ready);
		if (err)
			continue;
		lz4_do_compression(inst);
	}
	return 0;
}

static int lz4_global_init(struct zcomp_params *params)
{
	int err = 0;
	struct lz4_global *newinst = NULL;

	mutex_lock(&lz4_global_lock);

	if (lz4_global_data) {
		kref_get(&lz4_global_data->ref);
		err = 0;
		goto out_unlock;
	}

	newinst = kvzalloc_obj(*newinst);
	if (!newinst) {
		err = -ENOMEM;
		goto out_unlock;
	}

	INIT_KFIFO(newinst->working_queue[0]);
	INIT_KFIFO(newinst->working_queue[1]);
	newinst->working_submit_idx = 0;

	mutex_init(&newinst->stream_lock);
	mutex_init(&newinst->working_lock);
	kref_init(&newinst->ref);
	err = lz4_streams_init(params, newinst);
	if (err)
		goto err_stream_init;
	init_completion(&newinst->new_task_ready);
	newinst->tsk = kthread_run(lz4_thread_worker, newinst, "zcomp_lz4");
	if (IS_ERR(newinst->tsk)) {
		err = PTR_ERR(newinst->tsk);
		goto err_kthread_init;
	}

	lz4_global_data = newinst;
	mutex_unlock(&lz4_global_lock);
	return 0;

err_kthread_init:
	lz4_streams_destroy(newinst);
err_stream_init:
	mutex_destroy(&newinst->working_lock);
	mutex_destroy(&newinst->stream_lock);
	kvfree(newinst);
out_unlock:
	mutex_unlock(&lz4_global_lock);
	return err;
}

static void lz4_global_destroy(struct kref *ref)
{
	struct lz4_global *inst;

	lockdep_assert_held(&lz4_global_lock);

	if (!lz4_global_data)
		return;
	inst = container_of(ref, struct lz4_global, ref);
	WARN_ON(inst != lz4_global_data);

	lz4_global_data = NULL;
	kthread_stop(inst->tsk);
	lz4_streams_destroy(inst);
	mutex_destroy(&inst->stream_lock);
	mutex_destroy(&inst->working_lock);
	kvfree(inst);
}

struct lz4_drv {
	struct mutex lock;
	DECLARE_KFIFO(ctxqueue, struct lz4_ctx *, BACKEND_LZ4_STREAM_MAX);
	struct lz4_ctx ctxs[BACKEND_LZ4_STREAM_MAX];
};

static int lz4_ctx_init(struct zcomp_params *params, struct lz4_ctx *zctx);
static void lz4_ctx_destroy(struct lz4_ctx *zctx);

static void lz4_drv_free(struct lz4_drv *drv_data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(drv_data->ctxs); i++)
		lz4_ctx_destroy(&drv_data->ctxs[i]);

	mutex_destroy(&drv_data->lock);

	kvfree(drv_data);
}

static int lz4_drv_alloc(struct zcomp_params *params)
{
	struct lz4_drv *drv_data = NULL;
	int i, len;
	int err = 0;

	drv_data = kvzalloc_obj(*drv_data);
	if (!drv_data)
		return -ENOMEM;
	mutex_init(&drv_data->lock);

	INIT_KFIFO(drv_data->ctxqueue);

	len = kfifo_size(&drv_data->ctxqueue);

	for (i = 0; i < min(len, ARRAY_SIZE(drv_data->ctxs)); i++) {
		struct lz4_ctx *ctx = &drv_data->ctxs[i];

		err = lz4_ctx_init(params, ctx);
		if (err)
			break;

		kfifo_put(&drv_data->ctxqueue, ctx);
	}

	if (err) {
		lz4_drv_free(drv_data);
		return err;
	}

	params->drv_data = drv_data;

	return 0;
}

static void lz4_release_params(struct zcomp_params *params)
{
	struct lz4_drv *drv_data = params->drv_data;

	if (!params->zstrm_mgmt)
		return;

	lz4_drv_free(drv_data);

	kref_put_mutex(&lz4_global_data->ref, lz4_global_destroy,
		       &lz4_global_lock);
}

static int lz4_setup_params(struct zcomp_params *params)
{
	int err = 0;

	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = LZ4_ACCELERATION_DEFAULT;

	params->zstrm_mgmt = false;
	err = lz4_global_init(params);
	if (err) {
		pr_err("lz4 global init failed: %d, managed stream disabled",
		       err);
		return 0;
	}

	err = lz4_drv_alloc(params);

	if (err) {
		pr_err("lz4 drv init failed: %d, managed stream disabled", err);
		kref_put_mutex(&lz4_global_data->ref, lz4_global_destroy,
			       &lz4_global_lock);
		return 0;
	}

	params->zstrm_mgmt = true;
	return 0;
}

static void lz4_ctx_destroy(struct lz4_ctx *zctx)
{
	vfree(zctx->mem);
	kfree(zctx->dstrm);
	kfree(zctx->cstrm);
}

static void lz4_destroy(struct zcomp_ctx *ctx)
{
	struct lz4_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	lz4_ctx_destroy(zctx);
	kfree(zctx);
}

static int lz4_ctx_init(struct zcomp_params *params, struct lz4_ctx *zctx)
{
	void *mem __free(kvfree) = NULL;
	LZ4_streamDecode_t *dstrm __free(kfree) = NULL;
	LZ4_stream_t *cstrm __free(kfree) = NULL;

	if (params->dict_sz == 0) {
		mem = vmalloc(LZ4_MEM_COMPRESS);
		if (!mem)
			return -ENOMEM;
	} else {
		dstrm = kzalloc_obj(*zctx->dstrm);
		if (!dstrm)
			return -ENOMEM;

		cstrm = kzalloc_obj(*zctx->cstrm);
		if (!cstrm)
			return -ENOMEM;
	}

	zctx->mem = no_free_ptr(mem);
	zctx->dstrm = no_free_ptr(dstrm);
	zctx->cstrm = no_free_ptr(cstrm);
	return 0;
}

static int lz4_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct lz4_ctx *zctx;

	zctx = kzalloc_obj(*zctx);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;
	if (lz4_ctx_init(params, zctx)) {
		lz4_destroy(ctx);
		return -ENOMEM;
	}

	return 0;
}

static int __lz4_compress(struct zcomp_params *params, struct lz4_ctx *zctx,
			  struct zcomp_req *req)
{
	int ret;

	if (!zctx->cstrm) {
		ret = LZ4_compress_fast(req->src, req->dst, req->src_len,
					req->dst_len, params->level,
					zctx->mem);
	} else {
		/* Cstrm needs to be reset */
		ret = LZ4_loadDict(zctx->cstrm, params->dict, params->dict_sz);
		if (ret != params->dict_sz)
			return -EINVAL;
		ret = LZ4_compress_fast_continue(zctx->cstrm, req->src,
						 req->dst, req->src_len,
						 req->dst_len, params->level);
	}
	if (!ret)
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int lz4_compress_managed(struct zcomp_params *params,
				struct lz4_ctx *zctx, struct zcomp_req *req,
				struct zcomp_strm *zstrm)
{
	struct lz4_stream *mngt_strm =
		container_of(zstrm_to_managed(zstrm), struct lz4_stream, zstrm);

	scoped_guard(mutex, &lz4_global_data->working_lock)
	{
		int cnt;
		int idx = lz4_global_data->working_submit_idx;
		struct lz4_req lz4req = {
			.strm = mngt_strm,
			.params = params,
			.ctx = zctx,
			.zreq = req
		};

		/* ctx->src is mapped by kmap_local_map() */
		BUILD_BUG_ON(IS_ENABLED(CONFIG_HIGHMEM));
		cnt = kfifo_put(&lz4_global_data->working_queue[idx], lz4req);
		if (cnt == 0)
			return -EBUSY;
	}
	complete(&lz4_global_data->new_task_ready);
	wait_for_completion(&mngt_strm->completion);
	return mngt_strm->result;
}

static int lz4_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req)
{
	struct lz4_ctx *zctx = ctx->context;
	struct zcomp_strm *zstrm = container_of(ctx, struct zcomp_strm, ctx);

	if (!zstrm->zcomp_managed)
		return __lz4_compress(params, zctx, req);

	return lz4_compress_managed(params, zctx, req, zstrm);
}

static int lz4_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	struct lz4_ctx *zctx = ctx->context;
	int ret;

	if (!zctx->dstrm) {
		ret = LZ4_decompress_safe(req->src, req->dst, req->src_len,
					  req->dst_len);
	} else {
		/* Dstrm needs to be reset */
		ret = LZ4_setStreamDecode(zctx->dstrm, params->dict,
					  params->dict_sz);
		if (!ret)
			return -EINVAL;
		ret = LZ4_decompress_safe_continue(zctx->dstrm, req->src,
						   req->dst, req->src_len,
						   req->dst_len);
	}
	if (ret < 0)
		return -EINVAL;
	return 0;
}

static struct managed_zstrm *lz4_get_stream(struct zcomp_params *params)
{
	struct lz4_stream *lz4_strm;
	struct lz4_drv *drv_data = params->drv_data;
	struct lz4_ctx *ctx;

	if (!params->zstrm_mgmt)
		return NULL;

	scoped_guard(mutex, &lz4_global_data->stream_lock)
	{
		lz4_strm = list_first_entry_or_null(
			&lz4_global_data->stream_head, struct lz4_stream, node);
		if (!lz4_strm)
			return NULL;

		list_del_init(&lz4_strm->node);
	}

	scoped_guard(mutex, &drv_data->lock)
		if (!kfifo_get(&drv_data->ctxqueue, &ctx))
			ctx = NULL;
	if (!ctx) {
		guard(mutex)(&lz4_global_data->stream_lock);
		list_add(&lz4_strm->node, &lz4_global_data->stream_head);
		return NULL;
	}
	reinit_completion(&lz4_strm->completion);
	lz4_strm->zstrm.strm.ctx.context = ctx;

	return &lz4_strm->zstrm;
}

static void lz4_put_stream(struct zcomp_params *params,
			   struct managed_zstrm *zstrm)
{
	struct lz4_stream *lz4_strm;
	struct lz4_ctx *ctx;
	struct lz4_drv *drv_data = params->drv_data;

	if (!zstrm)
		return;
	if (WARN_ON(!params->zstrm_mgmt))
		return;

	lz4_strm = container_of(zstrm, struct lz4_stream, zstrm);
	ctx = zstrm->strm.ctx.context;
	lz4_strm->zstrm.strm.ctx.context = NULL;

	scoped_guard(mutex, &lz4_global_data->stream_lock)
		list_add(&lz4_strm->node, &lz4_global_data->stream_head);
	scoped_guard(mutex, &drv_data->lock)
		kfifo_put(&drv_data->ctxqueue, ctx);
}

const struct zcomp_ops backend_lz4 = {
	.compress	= lz4_compress,
	.decompress	= lz4_decompress,
	.create_ctx	= lz4_create,
	.destroy_ctx	= lz4_destroy,
	.setup_params	= lz4_setup_params,
	.release_params	= lz4_release_params,
	.get_stream	= lz4_get_stream,
	.put_stream	= lz4_put_stream,
	.name		= "lz4",
};
