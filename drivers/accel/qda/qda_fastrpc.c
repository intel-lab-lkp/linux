// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sort.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <drm/drm_gem.h>
#include <drm/qda_accel.h>
#include "qda_fastrpc.h"
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"

static int copy_from_user_or_kernel(void *dst, const void __user *src, size_t size)
{
	if ((unsigned long)src >= PAGE_OFFSET) {
		memcpy(dst, src, size);
		return 0;
	} else {
		return copy_from_user(dst, src, size) ? -EFAULT : 0;
	}
}

static int copy_to_user_or_kernel(void __user *dst, const void *src, size_t size)
{
	if ((unsigned long)dst >= PAGE_OFFSET) {
		memcpy(dst, src, size);
		return 0;
	} else {
		return copy_to_user(dst, src, size) ? -EFAULT : 0;
	}
}

static int get_gem_obj_from_handle(struct drm_file *file_priv, u32 handle,
				   struct drm_gem_object **gem_obj)
{
	if (handle == 0)
		return -EINVAL;

	if (!file_priv)
		return -EINVAL;

	*gem_obj = drm_gem_object_lookup(file_priv, handle);
	if (*gem_obj)
		return 0;

	return -ENOENT;
}

static void setup_pages_from_gem_obj(struct qda_gem_obj *qda_gem_obj,
				     struct fastrpc_phy_page *pages)
{
	if (qda_gem_obj->is_imported)
		pages->addr = qda_gem_obj->imported_dma_addr;
	else
		pages->addr = qda_gem_obj->dma_addr;

	pages->size = qda_gem_obj->size;
}

static u64 calculate_vma_offset(u64 user_ptr)
{
	struct vm_area_struct *vma;
	u64 user_ptr_page_mask = user_ptr & PAGE_MASK;
	u64 vma_offset = 0;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, user_ptr);
	if (vma)
		vma_offset = user_ptr_page_mask - vma->vm_start;
	mmap_read_unlock(current->mm);

	return vma_offset;
}

static u64 calculate_page_aligned_size(u64 ptr, u64 len)
{
	u64 pg_start = (ptr & PAGE_MASK) >> PAGE_SHIFT;
	u64 pg_end = ((ptr + len - 1) & PAGE_MASK) >> PAGE_SHIFT;
	u64 aligned_size = (pg_end - pg_start + 1) * PAGE_SIZE;

	return aligned_size;
}

static void setup_single_arg(struct fastrpc_invoke_args *args, void *ptr, size_t size)
{
	args[0].ptr = (u64)(uintptr_t)ptr;
	args[0].length = size;
	args[0].fd = -1;
}

static struct fastrpc_invoke_buf *fastrpc_invoke_buf_start(union fastrpc_remote_arg *pra, int len)
{
	struct fastrpc_invoke_buf *buf = (struct fastrpc_invoke_buf *)(&pra[len]);
	return buf;
}

static struct fastrpc_phy_page *fastrpc_phy_page_start(struct fastrpc_invoke_buf *buf, int len)
{
	struct fastrpc_phy_page *pages = (struct fastrpc_phy_page *)(&buf[len]);
	return pages;
}

static int fastrpc_get_meta_size(struct fastrpc_invoke_context *ctx)
{
	int size = 0;

	size = (sizeof(struct fastrpc_remote_buf) +
		sizeof(struct fastrpc_invoke_buf) +
		sizeof(struct fastrpc_phy_page)) * ctx->nscalars +
		sizeof(u64) * FASTRPC_MAX_FDLIST +
		sizeof(u32) * FASTRPC_MAX_CRCLIST;

	return size;
}

static u64 fastrpc_get_payload_size(struct fastrpc_invoke_context *ctx, int metalen)
{
	u64 size = 0;
	int oix;

	size = ALIGN(metalen, FASTRPC_ALIGN);

	for (oix = 0; oix < ctx->nbufs; oix++) {
		int i = ctx->olaps[oix].raix;

		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1) {
			if (ctx->olaps[oix].offset == 0)
				size = ALIGN(size, FASTRPC_ALIGN);

			size += (ctx->olaps[oix].mend - ctx->olaps[oix].mstart);
		}
	}

	return size;
}

void fastrpc_context_free(struct kref *ref)
{
	struct fastrpc_invoke_context *ctx;
	int i;

	ctx = container_of(ref, struct fastrpc_invoke_context, refcount);
	if (ctx->gem_objs) {
		for (i = 0; i < ctx->nscalars; ++i) {
			if (ctx->gem_objs[i]) {
				drm_gem_object_put(ctx->gem_objs[i]);
				ctx->gem_objs[i] = NULL;
			}
		}
		kfree(ctx->gem_objs);
		ctx->gem_objs = NULL;
	}

	if (ctx->msg_gem_obj) {
		drm_gem_object_put(&ctx->msg_gem_obj->base);
		ctx->msg_gem_obj = NULL;
	}

	kfree(ctx->olaps);
	ctx->olaps = NULL;

	kfree(ctx->args);
	kfree(ctx->req);
	kfree(ctx->rsp);
	kfree(ctx->input_pages);
	kfree(ctx->inbuf);

	kfree(ctx);
}

#define CMP(aa, bb) ((aa) == (bb) ? 0 : (aa) < (bb) ? -1 : 1)

static int olaps_cmp(const void *a, const void *b)
{
	struct fastrpc_buf_overlap *pa = (struct fastrpc_buf_overlap *)a;
	struct fastrpc_buf_overlap *pb = (struct fastrpc_buf_overlap *)b;
	int st = CMP(pa->start, pb->start);
	int ed = CMP(pb->end, pa->end);

	return st == 0 ? ed : st;
}

static void fastrpc_get_buff_overlaps(struct fastrpc_invoke_context *ctx)
{
	u64 max_end = 0;
	int i;

	for (i = 0; i < ctx->nbufs; ++i) {
		ctx->olaps[i].start = ctx->args[i].ptr;
		ctx->olaps[i].end = ctx->olaps[i].start + ctx->args[i].length;
		ctx->olaps[i].raix = i;
	}

	sort(ctx->olaps, ctx->nbufs, sizeof(*ctx->olaps), olaps_cmp, NULL);

	for (i = 0; i < ctx->nbufs; ++i) {
		if (ctx->olaps[i].start < max_end) {
			ctx->olaps[i].mstart = max_end;
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].offset = max_end - ctx->olaps[i].start;

			if (ctx->olaps[i].end > max_end) {
				max_end = ctx->olaps[i].end;
			} else {
				ctx->olaps[i].mend = 0;
				ctx->olaps[i].mstart = 0;
			}
		} else {
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].mstart = ctx->olaps[i].start;
			ctx->olaps[i].offset = 0;
			max_end = ctx->olaps[i].end;
		}
	}
}

struct fastrpc_invoke_context *fastrpc_context_alloc(void)
{
	struct fastrpc_invoke_context *ctx = NULL;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ctx->node);

	ctx->retval = -1;
	ctx->pid = current->pid;
	init_completion(&ctx->work);
	ctx->msg_gem_obj = NULL;
	kref_init(&ctx->refcount);

	return ctx;
}

static int process_fd_buffer(struct fastrpc_invoke_context *ctx, int i,
			     union fastrpc_remote_arg *rpra, struct fastrpc_phy_page *pages)
{
	struct drm_gem_object *gem_obj;
	struct qda_gem_obj *qda_gem_obj;
	int err;
	u64 len = ctx->args[i].length;
	u64 vma_offset;

	err = get_gem_obj_from_handle(ctx->file_priv, ctx->args[i].fd, &gem_obj);
	if (err)
		return err;

	ctx->gem_objs[i] = gem_obj;
	qda_gem_obj = to_qda_gem_obj(gem_obj);

	rpra[i].buf.pv = (u64)ctx->args[i].ptr;

	if (qda_gem_obj->is_imported)
		pages[i].addr = qda_gem_obj->imported_dma_addr;
	else
		pages[i].addr = qda_gem_obj->dma_addr;

	vma_offset = calculate_vma_offset(ctx->args[i].ptr);
	pages[i].addr += vma_offset;
	pages[i].size = calculate_page_aligned_size(ctx->args[i].ptr, len);

	return 0;
}

static int process_direct_buffer(struct fastrpc_invoke_context *ctx, int i, int oix,
				 union fastrpc_remote_arg *rpra, struct fastrpc_phy_page *pages,
				 uintptr_t *args, u64 *rlen, u64 pkt_size)
{
	int mlen;
	u64 len = ctx->args[i].length;
	int inbufs = ctx->inbufs;

	if (ctx->olaps[oix].offset == 0) {
		*rlen -= ALIGN(*args, FASTRPC_ALIGN) - *args;
		*args = ALIGN(*args, FASTRPC_ALIGN);
	}

	mlen = ctx->olaps[oix].mend - ctx->olaps[oix].mstart;

	if (*rlen < mlen)
		return -ENOSPC;

	rpra[i].buf.pv = *args - ctx->olaps[oix].offset;

	pages[i].addr = ctx->msg->phys - ctx->olaps[oix].offset + (pkt_size - *rlen);
	pages[i].addr = pages[i].addr & PAGE_MASK;
	pages[i].size = calculate_page_aligned_size(rpra[i].buf.pv, len);

	*args = *args + mlen;
	*rlen -= mlen;

	if (i < inbufs) {
		void *dst = (void *)(uintptr_t)rpra[i].buf.pv;
		void *src = (void *)(uintptr_t)ctx->args[i].ptr;

		if ((unsigned long)src >= PAGE_OFFSET) {
			memcpy(dst, src, len);
		} else {
			if (copy_from_user(dst, (void __user *)src, len))
				return -EFAULT;
		}
	}

	return 0;
}

static int process_dma_handle(struct fastrpc_invoke_context *ctx, int i,
			      union fastrpc_remote_arg *rpra, struct fastrpc_phy_page *pages)
{
	if (ctx->args[i].fd > 0) {
		struct drm_gem_object *gem_obj;
		struct qda_gem_obj *qda_gem_obj;
		int err;

		err = get_gem_obj_from_handle(ctx->file_priv, ctx->args[i].fd, &gem_obj);
		if (err)
			return err;

		ctx->gem_objs[i] = gem_obj;
		qda_gem_obj = to_qda_gem_obj(gem_obj);

		setup_pages_from_gem_obj(qda_gem_obj, &pages[i]);

		rpra[i].dma.fd = ctx->args[i].fd;
		rpra[i].dma.len = ctx->args[i].length;
		rpra[i].dma.offset = (u64)ctx->args[i].ptr;
	} else {
		rpra[i].buf.pv = ctx->args[i].ptr;
		rpra[i].buf.len = ctx->args[i].length;
	}

	return 0;
}

int fastrpc_get_header_size(struct fastrpc_invoke_context *ctx, size_t *out_size)
{
	ctx->inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	ctx->metalen = fastrpc_get_meta_size(ctx);
	ctx->pkt_size = fastrpc_get_payload_size(ctx, ctx->metalen);

	ctx->aligned_pkt_size = PAGE_ALIGN(ctx->pkt_size);
	if (ctx->aligned_pkt_size == 0)
		return -EINVAL;

	*out_size = ctx->aligned_pkt_size;
	return 0;
}

static int fastrpc_get_args(struct fastrpc_invoke_context *ctx)
{
	union fastrpc_remote_arg *rpra;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	int i, oix, err = 0;
	u64 rlen;
	uintptr_t args;
	size_t hdr_size;

	ctx->inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	err = fastrpc_get_header_size(ctx, &hdr_size);
	if (err)
		return err;

	ctx->msg->buf = ctx->msg_gem_obj->virt;
	ctx->msg->phys = ctx->msg_gem_obj->dma_addr;

	memset(ctx->msg->buf, 0, ctx->aligned_pkt_size);

	rpra = (union fastrpc_remote_arg *)ctx->msg->buf;
	ctx->list = fastrpc_invoke_buf_start(rpra, ctx->nscalars);
	ctx->pages = fastrpc_phy_page_start(ctx->list, ctx->nscalars);
	list = ctx->list;
	pages = ctx->pages;
	args = (uintptr_t)ctx->msg->buf + ctx->metalen;
	rlen = ctx->pkt_size - ctx->metalen;
	ctx->rpra = rpra;

	for (oix = 0; oix < ctx->nbufs; ++oix) {
		i = ctx->olaps[oix].raix;

		rpra[i].buf.pv = 0;
		rpra[i].buf.len = ctx->args[i].length;
		list[i].num = ctx->args[i].length ? 1 : 0;
		list[i].pgidx = i;

		if (!ctx->args[i].length)
			continue;

		if (ctx->args[i].fd > 0)
			err = process_fd_buffer(ctx, i, rpra, pages);
		else
			err = process_direct_buffer(ctx, i, oix, rpra, pages, &args, &rlen,
						    ctx->pkt_size);

		if (err)
			goto bail_gem;
	}

	for (i = ctx->nbufs; i < ctx->nscalars; ++i) {
		list[i].num = ctx->args[i].length ? 1 : 0;
		list[i].pgidx = i;

		err = process_dma_handle(ctx, i, rpra, pages);
		if (err)
			goto bail_gem;
	}

	return 0;

bail_gem:
	if (ctx->msg_gem_obj) {
		drm_gem_object_put(&ctx->msg_gem_obj->base);
		ctx->msg_gem_obj = NULL;
	}

	return err;
}

static int fastrpc_put_args(struct fastrpc_invoke_context *ctx, struct qda_msg *msg)
{
	union fastrpc_remote_arg *rpra = ctx->rpra;
	int i, err = 0;

	if (!ctx || !rpra)
		return -EINVAL;

	for (i = ctx->inbufs; i < ctx->nbufs; ++i) {
		if (ctx->args[i].fd <= 0) {
			void *src = (void *)(uintptr_t)rpra[i].buf.pv;
			void *dst = (void *)(uintptr_t)ctx->args[i].ptr;
			u64 len = rpra[i].buf.len;

			err = copy_to_user_or_kernel(dst, src, len);
			if (err)
				break;
		}
	}

	return err;
}

int fastrpc_internal_invoke_pack(struct fastrpc_invoke_context *ctx,
				 struct qda_msg *msg)
{
	int err = 0;

	if (ctx->handle == FASTRPC_INIT_HANDLE)
		msg->client_id = 0;
	else
		msg->client_id = ctx->client_id;

	ctx->msg = msg;

	err = fastrpc_get_args(ctx);
	if (err)
		return err;

	dma_wmb();

	msg->tid = ctx->pid;
	msg->ctx = ctx->ctxid | ctx->pd;
	msg->handle = ctx->handle;
	msg->sc = ctx->sc;
	msg->addr = ctx->msg->phys;
	msg->size = roundup(ctx->pkt_size, PAGE_SIZE);
	msg->fastrpc_ctx = ctx;
	msg->file_priv = ctx->file_priv;

	return 0;
}

int fastrpc_internal_invoke_unpack(struct fastrpc_invoke_context *ctx,
				   struct qda_msg *msg)
{
	int err;

	dma_rmb();

	err = fastrpc_put_args(ctx, msg);
	if (err)
		return err;

	err = ctx->retval;
	return err;
}

static void setup_create_process_args(struct fastrpc_invoke_args *args,
				      struct fastrpc_create_process_inbuf *inbuf,
				      struct qda_init_create *init,
				      struct fastrpc_phy_page *pages)
{
	args[0].ptr = (u64)(uintptr_t)inbuf;
	args[0].length = sizeof(*inbuf);
	args[0].fd = -1;

	args[1].ptr = (u64)(uintptr_t)current->comm;
	args[1].length = inbuf->namelen;
	args[1].fd = -1;

	args[2].ptr = (u64)init->file;
	args[2].length = inbuf->filelen;
	args[2].fd = init->filefd;

	args[3].ptr = (u64)(uintptr_t)pages;
	args[3].length = 1 * sizeof(*pages);
	args[3].fd = -1;

	args[4].ptr = (u64)(uintptr_t)&inbuf->attrs;
	args[4].length = sizeof(inbuf->attrs);
	args[4].fd = -1;

	args[5].ptr = (u64)(uintptr_t)&inbuf->siglen;
	args[5].length = sizeof(inbuf->siglen);
	args[5].fd = -1;
}

static int fastrpc_prepare_args_init_attach(struct fastrpc_invoke_context *ctx)
{
	struct fastrpc_invoke_args *args;

	args = kzalloc_obj(*args, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	setup_single_arg(args, &ctx->client_id, sizeof(ctx->client_id));
	ctx->sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_ATTACH, 1, 0);
	ctx->args = args;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;
}

static int fastrpc_prepare_args_release_process(struct fastrpc_invoke_context *ctx)
{
	struct fastrpc_invoke_args *args;

	args = kzalloc_obj(*args, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	setup_single_arg(args, &ctx->client_id, sizeof(ctx->client_id));
	ctx->sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_RELEASE, 1, 0);
	ctx->args = args;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;
}

static int fastrpc_prepare_args_invoke(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct fastrpc_invoke_args *args = NULL;
	struct qda_invoke_args inv;
	int err = 0;
	int nscalars;

	if (!argp)
		return -EINVAL;

	err = copy_from_user_or_kernel(&inv, argp, sizeof(inv));
	if (err)
		return err;

	nscalars = REMOTE_SCALARS_LENGTH(inv.sc);

	if (nscalars) {
		args = kcalloc(nscalars, sizeof(*args), GFP_KERNEL);
		if (!args)
			return -ENOMEM;

		err = copy_from_user_or_kernel(args, (const void __user *)(uintptr_t)inv.args,
					       nscalars * sizeof(*args));
		if (err) {
			kfree(args);
			return err;
		}
	}
	ctx->sc = inv.sc;
	ctx->args = args;
	ctx->handle = inv.handle;

	return 0;
}

static int fastrpc_prepare_args_init_create(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct qda_init_create init;
	struct fastrpc_invoke_args *args;
	struct fastrpc_create_process_inbuf *inbuf;
	int err;
	u32 sc;
	struct drm_gem_object *file_gem_obj = NULL;

	args = kcalloc(FASTRPC_CREATE_PROCESS_NARGS, sizeof(*args), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	ctx->input_pages = kcalloc(1, sizeof(*ctx->input_pages), GFP_KERNEL);
	if (!ctx->input_pages) {
		err = -ENOMEM;
		goto err_free_args;
	}

	ctx->inbuf =  kcalloc(1, sizeof(*inbuf), GFP_KERNEL);
	if (!ctx->inbuf) {
		err = -ENOMEM;
		goto err_free_input_pages;
	}
	inbuf = ctx->inbuf;

	err = copy_from_user_or_kernel(&init, argp, sizeof(init));
	if (err)
		goto err_free_inbuf;

	if (init.filelen > INIT_FILELEN_MAX) {
		err = -EINVAL;
		goto err_free_inbuf;
	}
	inbuf->client_id = ctx->client_id;
	inbuf->namelen = strlen(current->comm) + 1;
	inbuf->filelen = init.filelen;
	inbuf->pageslen = 1;
	inbuf->attrs = init.attrs;
	inbuf->siglen = init.siglen;

	setup_pages_from_gem_obj(ctx->init_mem_gem_obj, &ctx->input_pages[0]);

	if (init.filelen && init.filefd) {
		err = get_gem_obj_from_handle(ctx->file_priv, init.filefd, &file_gem_obj);
		if (err) {
			err = -EINVAL;
			goto err_free_inbuf;
		}
		drm_gem_object_put(file_gem_obj);
	}

	setup_create_process_args(args, inbuf, &init, ctx->input_pages);

	sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE, 4, 0);
	if (init.attrs)
		sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE_ATTR, 4, 0);
	ctx->sc = sc;
	ctx->args = args;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;

err_free_inbuf:
	kfree(ctx->inbuf);
	ctx->inbuf = NULL;
err_free_input_pages:
	kfree(ctx->input_pages);
	ctx->input_pages = NULL;
err_free_args:
	kfree(args);
	return err;
}

int fastrpc_prepare_args(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	int err;

	switch (ctx->type) {
	case FASTRPC_RMID_INIT_ATTACH:
		ctx->pd = ROOT_PD;
		err = fastrpc_prepare_args_init_attach(ctx);
		break;
	case FASTRPC_RMID_INIT_RELEASE:
		err = fastrpc_prepare_args_release_process(ctx);
		break;
	case FASTRPC_RMID_INVOKE_DYNAMIC:
		err = fastrpc_prepare_args_invoke(ctx, argp);
		break;
	case FASTRPC_RMID_INIT_CREATE:
	case FASTRPC_RMID_INIT_CREATE_ATTR:
		ctx->pd = USER_PD;
		err = fastrpc_prepare_args_init_create(ctx, argp);
		break;
	default:
		return -EINVAL;
	}

	if (err)
		return err;

	ctx->nscalars = REMOTE_SCALARS_LENGTH(ctx->sc);
	ctx->nbufs = REMOTE_SCALARS_INBUFS(ctx->sc) + REMOTE_SCALARS_OUTBUFS(ctx->sc);

	if (ctx->nscalars) {
		ctx->gem_objs = kcalloc(ctx->nscalars, sizeof(*ctx->gem_objs), GFP_KERNEL);
		if (!ctx->gem_objs)
			return -ENOMEM;
		ctx->olaps = kcalloc(ctx->nscalars, sizeof(*ctx->olaps), GFP_KERNEL);
		if (!ctx->olaps) {
			kfree(ctx->gem_objs);
			ctx->gem_objs = NULL;
			return -ENOMEM;
		}
		fastrpc_get_buff_overlaps(ctx);
	}

	return err;
}
