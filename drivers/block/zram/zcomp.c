// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/list.h>

#include "zcomp.h"

#include "backend_lzo.h"
#include "backend_lzorle.h"
#include "backend_lz4.h"
#include "backend_lz4hc.h"
#include "backend_zstd.h"
#include "backend_deflate.h"
#include "backend_842.h"

static LIST_HEAD(backends);

static void zcomp_strm_free(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	comp->ops->destroy_ctx(&zstrm->ctx);
	vfree(zstrm->buffer);
	zstrm->buffer = NULL;
}

static int zcomp_strm_init(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	int ret;

	ret = comp->ops->create_ctx(comp, &zstrm->ctx);
	if (ret)
		return ret;

	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
	if (!zstrm->buffer) {
		zcomp_strm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static const struct zcomp_ops *lookup_backend_ops(const char *comp)
{
	struct zcomp_ops *backend;

	list_for_each_entry(backend, &backends, list)
		if (sysfs_streq(comp, backend->name))
			return backend;

	return NULL;
}

bool zcomp_available_algorithm(const char *comp)
{
	return lookup_backend_ops(comp) != NULL;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	ssize_t sz = 0;
	struct zcomp_ops *backend;

	list_for_each_entry(backend, &backends, list) {
		if (!strcmp(comp, backend->name)) {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backend->name);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backend->name);
		}
	}

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	local_lock(&comp->stream->lock);
	return this_cpu_ptr(comp->stream);
}

void zcomp_stream_put(struct zcomp *comp)
{
	local_unlock(&comp->stream->lock);
}

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		   const void *src, unsigned int *dst_len)
{
	struct zcomp_req req = {
		.src = src,
		.dst = zstrm->buffer,
		.src_len = PAGE_SIZE,
		.dst_len = 2 * PAGE_SIZE,
	};
	int ret;

	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (!ret)
		*dst_len = req.dst_len;
	return ret;
}

int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
		     const void *src, unsigned int src_len, void *dst)
{
	struct zcomp_req req = {
		.src = src,
		.dst = dst,
		.src_len = src_len,
		.dst_len = PAGE_SIZE,
	};

	return comp->ops->decompress(comp->params, &zstrm->ctx, &req);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm;
	int ret;

	zstrm = per_cpu_ptr(comp->stream, cpu);
	local_lock_init(&zstrm->lock);

	ret = zcomp_strm_init(comp, zstrm);
	if (ret)
		pr_err("Can't allocate a compression stream\n");
	return ret;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm;

	zstrm = per_cpu_ptr(comp->stream, cpu);
	zcomp_strm_free(comp, zstrm);
	return 0;
}

static int zcomp_init(struct zcomp *comp, struct zcomp_params *params)
{
	int ret;

	comp->stream = alloc_percpu(struct zcomp_strm);
	if (!comp->stream)
		return -ENOMEM;

	comp->params = params;
	ret = comp->ops->setup_params(comp->params);
	if (ret)
		goto cleanup;

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;

	return 0;

cleanup:
	comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	kfree(comp);
}

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params)
{
	struct zcomp *comp;
	int error;

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->ops = lookup_backend_ops(alg);
	if (!comp->ops) {
		kfree(comp);
		return ERR_PTR(-EINVAL);
	}

	error = zcomp_init(comp, params);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}

void clean_zcomp_backends(void)
{
	struct zcomp_ops *backend;

	list_for_each_entry(backend, &backends, list)
		backend->destroy(backend);
}

int init_zcomp_backends(void)
{
	struct zcomp_ops *ops;

#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZO)
	ops = get_backend_lzorle();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);

	ops = get_backend_lzo();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4)
	ops = get_backend_lz4();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4HC)
	ops = get_backend_lz4hc();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_ZSTD)
	ops = get_backend_zstd();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_DEFLATE)
	ops = get_backend_deflate();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_842)
	ops = get_backend_842();
	if (IS_ERR_OR_NULL(ops))
		goto err;

	list_add(&ops->list, &backends);
#endif

	return 0;

err:
	clean_zcomp_backends();

	return PTR_ERR(ops);
}
