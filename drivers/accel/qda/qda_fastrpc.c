// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sort.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <drm/drm_gem.h>
#include "qda_fastrpc.h"
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"
#include "qda_prime.h"

/**
 * get_gem_obj_from_dmabuf_fd() - Import a DMA-BUF fd and return the GEM object
 * @ctx:       FastRPC invocation context
 * @dmabuf_fd: DMA-BUF file descriptor supplied by user space
 * @gem_obj:   Output GEM object (caller must call drm_gem_object_put() when done)
 *
 * Imports the DMA-BUF fd into the QDA device via qda_prime_fd_to_handle()
 * (which performs IOMMU device assignment for newly imported buffers) and
 * then looks up the resulting GEM object.  The caller is responsible for
 * calling drm_gem_object_put() on the returned object.
 *
 * Return: 0 on success, negative error code on failure
 */
static int get_gem_obj_from_dmabuf_fd(struct fastrpc_invoke_context *ctx,
				      int dmabuf_fd,
				      struct drm_gem_object **gem_obj)
{
	struct drm_device *dev = ctx->file_priv->minor->dev;
	u32 handle;
	int ret;

	ret = qda_prime_fd_to_handle(dev, ctx->file_priv, dmabuf_fd, &handle);
	if (ret)
		return ret;

	*gem_obj = drm_gem_object_lookup(ctx->file_priv, handle);
	if (!*gem_obj)
		return -ENOENT;

	return 0;
}

static void setup_pages_from_gem_obj(struct qda_gem_obj *qda_gem_obj,
				     struct fastrpc_phy_page *pages)
{
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

static struct fastrpc_invoke_buf *fastrpc_invoke_buf_start(union fastrpc_remote_arg *pra, int len)
{
	return (struct fastrpc_invoke_buf *)(&pra[len]);
}

static struct fastrpc_phy_page *fastrpc_phy_page_start(struct fastrpc_invoke_buf *buf, int len)
{
	return (struct fastrpc_phy_page *)(&buf[len]);
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

/**
 * qda_fastrpc_context_free() - Free an invocation context
 * @ref: Reference counter embedded in the context
 *
 * Called when the reference count reaches zero; releases all resources
 * associated with the invocation context.
 */
void qda_fastrpc_context_free(struct kref *ref)
{
	struct fastrpc_invoke_context *ctx;
	int i;

	ctx = container_of(ref, struct fastrpc_invoke_context, refcount);
	if (ctx->gem_objs) {
		for (i = 0; i < ctx->nscalars; ++i) {
			if (ctx->gem_objs[i])
				drm_gem_object_put(ctx->gem_objs[i]);
		}
		kfree(ctx->gem_objs);
	}

	if (ctx->msg_gem_obj)
		drm_gem_object_put(&ctx->msg_gem_obj->base);

	kfree(ctx->olaps);

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
	/* sort with lowest starting buffer first */
	int st = CMP(pa->start, pb->start);
	/* sort with highest ending buffer first */
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

/**
 * qda_fastrpc_context_alloc() - Allocate a new FastRPC invocation context
 *
 * Return: Pointer to allocated context, or ERR_PTR on failure
 */
struct fastrpc_invoke_context *qda_fastrpc_context_alloc(void)
{
	struct fastrpc_invoke_context *ctx = NULL;

	ctx = kzalloc_obj(*ctx);
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

/*
 * process_fd_buffer() - Handle an in/out buffer argument backed by a DMA-BUF fd
 *
 * args[i].fd is a DMA-BUF fd.  We import it to obtain the GEM object and its
 * IOMMU-mapped dma_addr for the physical page descriptor.  The DSP uses the
 * physical address directly for this buffer type; the fd is not forwarded.
 */
static int process_fd_buffer(struct fastrpc_invoke_context *ctx, int i,
			     union fastrpc_remote_arg *rpra, struct fastrpc_phy_page *pages)
{
	struct drm_gem_object *gem_obj;
	struct qda_gem_obj *qda_gem_obj;
	int err;
	u64 len = ctx->args[i].length;
	u64 vma_offset;

	err = get_gem_obj_from_dmabuf_fd(ctx, ctx->args[i].fd, &gem_obj);
	if (err)
		return err;

	ctx->gem_objs[i] = gem_obj;
	qda_gem_obj = to_qda_gem_obj(gem_obj);

	rpra[i].buf.pv = (u64)ctx->args[i].ptr;

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

		/*
		 * For user-space invocations (INVOKE_DYNAMIC), ptr is a user
		 * virtual address and must be copied safely. For all other
		 * (kernel-internal) invocations, ptr is a kernel address set
		 * by the driver itself and can be copied directly.
		 */
		if (ctx->type == FASTRPC_RMID_INVOKE_DYNAMIC) {
			if (copy_from_user(dst, (void __user *)src, len))
				return -EFAULT;
		} else {
			memcpy(dst, src, len);
		}
	}

	return 0;
}

/*
 * process_dma_handle() - Handle a DMA-handle scalar argument
 *
 * args[i].fd is a DMA-BUF fd.  We import it to get the physical page
 * descriptor for the kernel, but forward the original DMA-BUF fd to the
 * DSP in rpra[i].dma.fd so the DSP can identify the buffer by its fd.
 */
static int process_dma_handle(struct fastrpc_invoke_context *ctx, int i,
			      union fastrpc_remote_arg *rpra, struct fastrpc_phy_page *pages)
{
	if (ctx->args[i].fd > 0) {
		struct drm_gem_object *gem_obj;
		struct qda_gem_obj *qda_gem_obj;
		int err;

		err = get_gem_obj_from_dmabuf_fd(ctx, ctx->args[i].fd, &gem_obj);
		if (err)
			return err;

		ctx->gem_objs[i] = gem_obj;
		qda_gem_obj = to_qda_gem_obj(gem_obj);

		setup_pages_from_gem_obj(qda_gem_obj, &pages[i]);

		/* Forward the original DMA-BUF fd to the DSP */
		rpra[i].dma.fd     = ctx->args[i].fd;
		rpra[i].dma.len    = ctx->args[i].length;
		rpra[i].dma.offset = (u64)ctx->args[i].ptr;
	} else {
		rpra[i].buf.pv  = ctx->args[i].ptr;
		rpra[i].buf.len = ctx->args[i].length;
	}

	return 0;
}

/**
 * qda_fastrpc_get_header_size() - Compute the FastRPC message header size
 * @ctx: FastRPC invocation context
 * @out_size: Pointer to store the aligned packet size in bytes
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_fastrpc_get_header_size(struct fastrpc_invoke_context *ctx, size_t *out_size)
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
	err = qda_fastrpc_get_header_size(ctx, &hdr_size);
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
	union fastrpc_remote_arg *rpra;
	int i, err = 0;

	if (!ctx)
		return -EINVAL;

	rpra = ctx->rpra;
	if (!rpra)
		return -EINVAL;

	for (i = ctx->inbufs; i < ctx->nbufs; ++i) {
		if (ctx->args[i].fd <= 0) {
			void *src = (void *)(uintptr_t)rpra[i].buf.pv;
			void *dst = (void *)(uintptr_t)ctx->args[i].ptr;
			u64 len = rpra[i].buf.len;

			if (ctx->type == FASTRPC_RMID_INVOKE_DYNAMIC)
				err = copy_to_user((void __user *)dst, src, len) ? -EFAULT : 0;
			else
				memcpy(dst, src, len);
			if (err)
				break;
		}
	}

	return err;
}

/**
 * qda_fastrpc_invoke_pack() - Pack an invocation context into a QDA message
 * @ctx: FastRPC invocation context
 * @msg: QDA message structure to pack into
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_fastrpc_invoke_pack(struct fastrpc_invoke_context *ctx,
			    struct qda_msg *msg)
{
	int err = 0;

	if (ctx->handle == FASTRPC_INIT_HANDLE)
		msg->fastrpc.remote_session_id = 0;
	else
		msg->fastrpc.remote_session_id = ctx->remote_session_id;

	ctx->msg = msg;

	err = fastrpc_get_args(ctx);
	if (err)
		return err;

	dma_wmb();

	msg->fastrpc.tid    = ctx->pid;
	msg->fastrpc.ctx    = ctx->ctxid | ctx->pd;
	msg->fastrpc.handle = ctx->handle;
	msg->fastrpc.sc     = ctx->sc;
	msg->fastrpc.addr   = ctx->msg->phys;
	msg->fastrpc.size   = roundup(ctx->pkt_size, PAGE_SIZE);
	msg->fastrpc_ctx    = ctx;
	msg->file_priv      = ctx->file_priv;

	return 0;
}

/**
 * qda_fastrpc_invoke_unpack() - Unpack a response message into an invocation context
 * @ctx: FastRPC invocation context
 * @msg: QDA message structure to unpack from
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_fastrpc_invoke_unpack(struct fastrpc_invoke_context *ctx,
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

static int fastrpc_return_result_mem_map(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct drm_qda_mem_map margs;
	struct fastrpc_map_rsp_msg *rsp_msg;

	rsp_msg = ctx->rsp;

	memcpy(&margs, argp, sizeof(margs));

	margs.vaddrout = rsp_msg->vaddrout;

	memcpy(argp, &margs, sizeof(margs));
	return 0;
}

/**
 * qda_fastrpc_return_result() - Return invocation result to user-space
 * @ctx: FastRPC invocation context
 * @argp: User-space pointer to write result into
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_fastrpc_return_result(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	int err = 0;

	switch (ctx->type) {
	case FASTRPC_RMID_INIT_MMAP:
	case FASTRPC_RMID_INIT_MEM_MAP:
		err = fastrpc_return_result_mem_map(ctx, argp);
		break;
	default:
		break;
	}

	return err;
}

static void setup_create_process_args(struct drm_qda_fastrpc_invoke_args *args,
				      struct fastrpc_create_process_inbuf *inbuf,
				      struct drm_qda_init_create *init,
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
	args[2].fd = init->filefd;	/* DMA-BUF fd forwarded to DSP */

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

static void setup_single_arg(struct drm_qda_fastrpc_invoke_args *args, const void *ptr, size_t size)
{
	args[0].ptr = (u64)(uintptr_t)ptr;
	args[0].length = size;
	args[0].fd = -1;
}

/*
 * setup_mmap_pages() - Resolve a DMA-BUF fd to a physical page descriptor
 *
 * Imports the DMA-BUF fd as a GEM object to obtain the IOMMU-mapped
 * dma_addr, fills in the fastrpc_phy_page entry, then releases the extra
 * GEM object reference.  The handle table keeps the object alive.
 */
static int setup_mmap_pages(struct fastrpc_invoke_context *ctx, int dmabuf_fd,
			    struct fastrpc_phy_page *pages)
{
	struct drm_gem_object *gem_obj;
	struct qda_gem_obj *qda_gem_obj;
	int err;

	if (dmabuf_fd <= 0) {
		pages->addr = 0;
		pages->size = 0;
		return 0;
	}

	err = get_gem_obj_from_dmabuf_fd(ctx, dmabuf_fd, &gem_obj);
	if (err)
		return err;

	qda_gem_obj = to_qda_gem_obj(gem_obj);
	setup_pages_from_gem_obj(qda_gem_obj, pages);

	drm_gem_object_put(gem_obj);
	return 0;
}

static int fastrpc_prepare_args_release_process(struct fastrpc_invoke_context *ctx)
{
	struct drm_qda_fastrpc_invoke_args *args;

	args = kzalloc_obj(*args);
	if (!args)
		return -ENOMEM;

	setup_single_arg(args, &ctx->remote_session_id, sizeof(ctx->remote_session_id));
	ctx->sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_RELEASE, 1, 0);
	ctx->args = args;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;
}

static int fastrpc_prepare_args_init_create(struct fastrpc_invoke_context *ctx,
					    char __user *argp)
{
	struct drm_qda_init_create init;
	struct drm_qda_fastrpc_invoke_args *args;
	struct fastrpc_create_process_inbuf *inbuf;
	int err;
	u32 sc;

	args = kcalloc(FASTRPC_CREATE_PROCESS_NARGS, sizeof(*args), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	ctx->input_pages = kcalloc(1, sizeof(*ctx->input_pages), GFP_KERNEL);
	if (!ctx->input_pages) {
		err = -ENOMEM;
		goto err_free_args;
	}

	ctx->inbuf = kcalloc(1, sizeof(*inbuf), GFP_KERNEL);
	if (!ctx->inbuf) {
		err = -ENOMEM;
		goto err_free_input_pages;
	}
	inbuf = ctx->inbuf;

	memcpy(&init, argp, sizeof(init));

	if (init.filelen > FASTRPC_INIT_FILELEN_MAX) {
		err = -EINVAL;
		goto err_free_inbuf;
	}

	/*
	 * Validate that the DMA-BUF fd is importable.  The fd itself is kept
	 * in init.filefd and forwarded to the DSP via setup_create_process_args().
	 */
	if (init.filelen && init.filefd > 0) {
		struct drm_gem_object *file_gem_obj;

		err = get_gem_obj_from_dmabuf_fd(ctx, init.filefd, &file_gem_obj);
		if (err) {
			err = -EINVAL;
			goto err_free_inbuf;
		}
		drm_gem_object_put(file_gem_obj);
	}

	inbuf->remote_session_id = ctx->remote_session_id;
	inbuf->namelen = strlen(current->comm) + 1;
	inbuf->filelen = init.filelen;
	inbuf->pageslen = 1;
	inbuf->attrs = init.attrs;
	inbuf->siglen = init.siglen;

	setup_pages_from_gem_obj(ctx->init_mem_gem_obj, &ctx->input_pages[0]);

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

static int fastrpc_prepare_args_map(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct drm_qda_mem_map margs;
	struct drm_qda_fastrpc_invoke_args *args;
	void *req, *rsp;
	struct fastrpc_map_req_msg *req_msg;
	struct fastrpc_map_rsp_msg *rsp_msg;
	int err;

	memcpy(&margs, argp, sizeof(margs));

	args = kzalloc_objs(*args, 3);
	if (!args)
		return -ENOMEM;

	req = kzalloc_obj(*req_msg);
	if (!req) {
		err = -ENOMEM;
		goto err_free_args;
	}
	req_msg = (struct fastrpc_map_req_msg *)req;

	rsp = kzalloc_obj(*rsp_msg);
	if (!rsp) {
		err = -ENOMEM;
		goto err_free_req;
	}
	rsp_msg = (struct fastrpc_map_rsp_msg *)rsp;

	ctx->input_pages = kzalloc_objs(*ctx->input_pages, 1);
	if (!ctx->input_pages) {
		err = -ENOMEM;
		goto err_free_rsp;
	}

	req_msg->remote_session_id = ctx->remote_session_id;
	req_msg->flags = margs.flags;
	req_msg->vaddr = margs.vaddrin;
	req_msg->num = sizeof(*ctx->input_pages);

	args[0].ptr = (u64)(uintptr_t)req;
	args[0].length = sizeof(*req_msg);
	args[0].fd = -1;

	/* Resolve DMA-BUF fd to physical page descriptor */
	err = setup_mmap_pages(ctx, margs.fd, ctx->input_pages);
	if (err)
		goto err_free_input_pages;

	args[1].ptr = (u64)(uintptr_t)ctx->input_pages;
	args[1].length = sizeof(*ctx->input_pages);
	args[1].fd = -1;

	args[2].ptr = (u64)(uintptr_t)rsp;
	args[2].length = sizeof(*rsp_msg);
	args[2].fd = -1;

	ctx->sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MMAP, 2, 1);
	ctx->args = args;
	ctx->req = req;
	ctx->rsp = rsp;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;

err_free_input_pages:
	kfree(ctx->input_pages);
	ctx->input_pages = NULL;
err_free_rsp:
	kfree(rsp);
err_free_req:
	kfree(req);
err_free_args:
	kfree(args);
	return err;
}

static int fastrpc_prepare_args_mem_map_attr(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct drm_qda_mem_map margs;
	struct drm_qda_fastrpc_invoke_args *args;
	void *req, *rsp;
	struct fastrpc_mem_map_req_msg *req_msg;
	struct fastrpc_map_rsp_msg *rsp_msg;
	int err;

	memcpy(&margs, argp, sizeof(margs));

	args = kzalloc_objs(*args, 4);
	if (!args)
		return -ENOMEM;

	req = kzalloc_obj(*req_msg);
	if (!req) {
		err = -ENOMEM;
		goto err_free_args;
	}
	req_msg = (struct fastrpc_mem_map_req_msg *)req;

	rsp = kzalloc_obj(*rsp_msg);
	if (!rsp) {
		err = -ENOMEM;
		goto err_free_req;
	}
	rsp_msg = (struct fastrpc_map_rsp_msg *)rsp;

	ctx->input_pages = kzalloc_objs(*ctx->input_pages, 1);
	if (!ctx->input_pages) {
		err = -ENOMEM;
		goto err_free_rsp;
	}

	req_msg->remote_session_id = ctx->remote_session_id;
	req_msg->fd       = margs.fd;		/* DMA-BUF fd forwarded to DSP */
	req_msg->offset   = margs.offset;
	req_msg->flags    = margs.flags;
	req_msg->vaddrin  = margs.vaddrin;
	req_msg->num      = sizeof(*ctx->input_pages);
	req_msg->data_len = 0;

	args[0].ptr = (u64)(uintptr_t)req;
	args[0].length = sizeof(*req_msg);
	args[0].fd = -1;

	/* Resolve DMA-BUF fd to physical page descriptor */
	err = setup_mmap_pages(ctx, margs.fd, ctx->input_pages);
	if (err)
		goto err_free_input_pages;

	args[1].ptr = (u64)(uintptr_t)ctx->input_pages;
	args[1].length = sizeof(*ctx->input_pages);
	args[1].fd = -1;

	/* args[2] is a zero-length handle-only entry required by the DSP protocol */
	args[2].ptr = (u64)(uintptr_t)ctx->input_pages;
	args[2].length = 0;
	args[2].fd = -1;

	args[3].ptr = (u64)(uintptr_t)rsp;
	args[3].length = sizeof(*rsp_msg);
	args[3].fd = -1;

	ctx->sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MEM_MAP, 3, 1);
	ctx->args = args;
	ctx->req = req;
	ctx->rsp = rsp;
	ctx->handle = FASTRPC_INIT_HANDLE;

	return 0;

err_free_input_pages:
	kfree(ctx->input_pages);
	ctx->input_pages = NULL;
err_free_rsp:
	kfree(rsp);
err_free_req:
	kfree(req);
err_free_args:
	kfree(args);
	return err;
}

static int fastrpc_prepare_args_invoke(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	struct drm_qda_invoke_args invoke_args;
	struct drm_qda_fastrpc_invoke_args *args = NULL;
	u32 nscalars;

	/* argp is DRM ioctl data (kernel pointer); args pointer within it is user-space */
	memcpy(&invoke_args, argp, sizeof(invoke_args));

	ctx->handle = invoke_args.handle;
	ctx->sc = invoke_args.sc;

	nscalars = REMOTE_SCALARS_LENGTH(ctx->sc);
	if (!nscalars) {
		ctx->args = NULL;
		return 0;
	}

	args = kcalloc(nscalars, sizeof(*args), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	if (copy_from_user(args, u64_to_user_ptr(invoke_args.args),
			   nscalars * sizeof(*args))) {
		kfree(args);
		return -EFAULT;
	}

	ctx->args = args;
	return 0;
}

/**
 * qda_fastrpc_prepare_args() - Prepare arguments for a FastRPC invocation
 * @ctx: FastRPC invocation context
 * @argp: User-space pointer to invocation arguments
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_fastrpc_prepare_args(struct fastrpc_invoke_context *ctx, char __user *argp)
{
	int err;

	switch (ctx->type) {
	case FASTRPC_RMID_INIT_RELEASE:
		err = fastrpc_prepare_args_release_process(ctx);
		break;
	case FASTRPC_RMID_INIT_CREATE:
	case FASTRPC_RMID_INIT_CREATE_ATTR:
		ctx->pd = QDA_USER_PD;
		err = fastrpc_prepare_args_init_create(ctx, argp);
		break;
	case FASTRPC_RMID_INIT_MMAP:
		err = fastrpc_prepare_args_map(ctx, argp);
		break;
	case FASTRPC_RMID_INIT_MEM_MAP:
		err = fastrpc_prepare_args_mem_map_attr(ctx, argp);
		break;
	case FASTRPC_RMID_INVOKE_DYNAMIC:
		err = fastrpc_prepare_args_invoke(ctx, argp);
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
