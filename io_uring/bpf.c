// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Red Hat */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <uapi/linux/io_uring.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/filter.h>
#include <linux/uio.h>
#include "io_uring.h"
#include "uring_bpf.h"
#include "rsrc.h"

#define MAX_BPF_OPS_COUNT	(1 << IORING_BPF_OP_BITS)

static DEFINE_MUTEX(uring_bpf_ctx_lock);
static LIST_HEAD(uring_bpf_ctx_list);
DEFINE_STATIC_SRCU(uring_bpf_srcu);
static struct uring_bpf_ops bpf_ops[MAX_BPF_OPS_COUNT];

static inline unsigned char uring_bpf_get_op(unsigned int op_flags)
{
	return (unsigned char)(op_flags >> IORING_BPF_OP_SHIFT);
}

static inline unsigned int uring_bpf_get_flags(unsigned int op_flags)
{
	return op_flags & IORING_BPF_CUSTOM_FLAGS_MASK;
}

static inline struct uring_bpf_ops *uring_bpf_get_ops(struct uring_bpf_data *data)
{
	return &bpf_ops[uring_bpf_get_op(data->opf)];
}

static int io_bpf_prep_buffers(struct io_kiocb *req,
			       const struct io_uring_sqe *sqe,
			       struct uring_bpf_data *data,
			       unsigned int op_flags)
{
	u8 buf1_type, buf2_type;

	/* Extract buffer configuration from bpf_op_flags */
	buf1_type = IORING_BPF_BUF1_TYPE(op_flags);
	buf2_type = IORING_BPF_BUF2_TYPE(op_flags);

	/* Prepare buffer 1 */
	if (buf1_type == IORING_BPF_BUF_TYPE_PLAIN) {
		/* Plain user buffer: addr=sqe->addr, len=sqe->len */
		data->buf1_addr = READ_ONCE(sqe->addr);
		data->buf1_len = READ_ONCE(sqe->len);
	} else if (buf1_type == IORING_BPF_BUF_TYPE_FIXED) {
		/* Fixed buffer: index=sqe->buf_index, offset=sqe->addr, len=sqe->len */
		req->buf_index = READ_ONCE(sqe->buf_index);
		data->buf1_addr = READ_ONCE(sqe->addr);  /* offset within fixed buffer */
		data->buf1_len = READ_ONCE(sqe->len);

		/* Validate buffer index */
		if (unlikely(!req->ctx->buf_table.nr))
			return -EFAULT;
		if (unlikely(req->buf_index >= req->ctx->buf_table.nr))
			return -EINVAL;
	} else if (buf1_type == IORING_BPF_BUF_TYPE_NONE) {
		data->buf1_addr = 0;
		data->buf1_len = 0;
	} else {
		return -EINVAL;
	}

	/* Prepare buffer 2 (plain only - io_uring only supports one fixed buffer) */
	if (buf2_type == IORING_BPF_BUF_TYPE_PLAIN) {
		/* Plain user buffer: addr=sqe->addr3, len=sqe->optlen */
		data->buf2_addr = READ_ONCE(sqe->addr3);
		data->buf2_len = READ_ONCE(sqe->optlen);
	} else if (buf2_type == IORING_BPF_BUF_TYPE_NONE) {
		data->buf2_addr = 0;
		data->buf2_len = 0;
	} else {
		return -EINVAL;
	}

	return 0;
}


int io_uring_bpf_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct uring_bpf_data *data = io_kiocb_to_cmd(req, struct uring_bpf_data);
	unsigned int op_flags = READ_ONCE(sqe->bpf_op_flags);
	struct uring_bpf_ops *ops;
	int ret;

	if (!(req->ctx->flags & IORING_SETUP_BPF))
		return -EACCES;

	if (uring_bpf_get_flags(op_flags))
		return -EINVAL;

	data->opf = op_flags;
	ops = &bpf_ops[uring_bpf_get_op(data->opf)];

	/* Prepare buffers based on buffer type flags */
	ret = io_bpf_prep_buffers(req, sqe, data, op_flags);
	if (ret)
		return ret;

	/* ctx->uring_lock is held */
	data->issue_flags = 0;
	if (ops->prep_fn)
		return ops->prep_fn(data, sqe);
	return -EOPNOTSUPP;
}

static int __io_uring_bpf_issue(struct io_kiocb *req)
{
	struct uring_bpf_data *data = io_kiocb_to_cmd(req, struct uring_bpf_data);
	struct uring_bpf_ops *ops = uring_bpf_get_ops(data);

	if (ops->issue_fn)
		return ops->issue_fn(data);
	return -ECANCELED;
}

int io_uring_bpf_issue(struct io_kiocb *req, unsigned int issue_flags)
{
	struct uring_bpf_data *data = io_kiocb_to_cmd(req, struct uring_bpf_data);

	data->issue_flags = issue_flags;
	if (issue_flags & IO_URING_F_UNLOCKED) {
		int idx, ret;

		idx = srcu_read_lock(&uring_bpf_srcu);
		ret = __io_uring_bpf_issue(req);
		srcu_read_unlock(&uring_bpf_srcu, idx);

		return ret;
	}
	return __io_uring_bpf_issue(req);
}

void io_uring_bpf_fail(struct io_kiocb *req)
{
	struct uring_bpf_data *data = io_kiocb_to_cmd(req, struct uring_bpf_data);
	struct uring_bpf_ops *ops = uring_bpf_get_ops(data);

	/* ctx->uring_lock is held */
	data->issue_flags = 0;
	if (ops->fail_fn)
		ops->fail_fn(data);
}

void io_uring_bpf_cleanup(struct io_kiocb *req)
{
	struct uring_bpf_data *data = io_kiocb_to_cmd(req, struct uring_bpf_data);
	struct uring_bpf_ops *ops = uring_bpf_get_ops(data);

	/* ctx->uring_lock is held */
	data->issue_flags = 0;
	if (ops->cleanup_fn)
		ops->cleanup_fn(data);
}

void uring_bpf_add_ctx(struct io_ring_ctx *ctx)
{
	guard(mutex)(&uring_bpf_ctx_lock);
	list_add(&ctx->bpf_node, &uring_bpf_ctx_list);
}

void uring_bpf_del_ctx(struct io_ring_ctx *ctx)
{
	guard(mutex)(&uring_bpf_ctx_lock);
	list_del(&ctx->bpf_node);
}

static const struct btf_type *uring_bpf_data_type;

static bool uring_bpf_ops_is_valid_access(int off, int size,
				       enum bpf_access_type type,
				       const struct bpf_prog *prog,
				       struct bpf_insn_access_aux *info)
{
	return bpf_tracing_btf_ctx_access(off, size, type, prog, info);
}

static int uring_bpf_ops_btf_struct_access(struct bpf_verifier_log *log,
					const struct bpf_reg_state *reg,
					int off, int size)
{
	const struct btf_type *t;

	t = btf_type_by_id(reg->btf, reg->btf_id);
	if (t != uring_bpf_data_type) {
		bpf_log(log, "only read is supported\n");
		return -EACCES;
	}

	if (off < offsetof(struct uring_bpf_data, pdu) ||
			off + size >= sizeof(struct uring_bpf_data))
		return -EACCES;

	return NOT_INIT;
}

static const struct bpf_verifier_ops io_bpf_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = uring_bpf_ops_is_valid_access,
	.btf_struct_access = uring_bpf_ops_btf_struct_access,
};

static int uring_bpf_ops_init(struct btf *btf)
{
	s32 type_id;

	type_id = btf_find_by_name_kind(btf, "uring_bpf_data", BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;
	uring_bpf_data_type = btf_type_by_id(btf, type_id);
	return 0;
}

static int uring_bpf_ops_check_member(const struct btf_type *t,
				   const struct btf_member *member,
				   const struct bpf_prog *prog)
{
	return 0;
}

static int uring_bpf_ops_init_member(const struct btf_type *t,
				 const struct btf_member *member,
				 void *kdata, const void *udata)
{
	const struct uring_bpf_ops *uuring_bpf_ops;
	struct uring_bpf_ops *kuring_bpf_ops;
	u32 moff;

	uuring_bpf_ops = (const struct uring_bpf_ops *)udata;
	kuring_bpf_ops = (struct uring_bpf_ops *)kdata;

	moff = __btf_member_bit_offset(t, member) / 8;

	switch (moff) {
	case offsetof(struct uring_bpf_ops, id):
		/* For dev_id, this function has to copy it and return 1 to
		 * indicate that the data has been handled by the struct_ops
		 * type, or the verifier will reject the map if the value of
		 * those fields is not zero.
		 */
		kuring_bpf_ops->id = uuring_bpf_ops->id;
		return 1;
	}
	return 0;
}

static int io_bpf_reg_unreg(struct uring_bpf_ops *ops, bool reg)
{
	struct io_ring_ctx *ctx;
	int ret = 0;

	guard(mutex)(&uring_bpf_ctx_lock);
	list_for_each_entry(ctx, &uring_bpf_ctx_list, bpf_node)
		mutex_lock(&ctx->uring_lock);

	if (reg) {
		if (bpf_ops[ops->id].issue_fn)
			ret = -EBUSY;
		else
			bpf_ops[ops->id] = *ops;
	} else {
		bpf_ops[ops->id] = (struct uring_bpf_ops) {0};
	}

	synchronize_srcu(&uring_bpf_srcu);

	list_for_each_entry(ctx, &uring_bpf_ctx_list, bpf_node)
		mutex_unlock(&ctx->uring_lock);

	return ret;
}

static int io_bpf_reg(void *kdata, struct bpf_link *link)
{
	struct uring_bpf_ops *ops = kdata;

	return io_bpf_reg_unreg(ops, true);
}

static void io_bpf_unreg(void *kdata, struct bpf_link *link)
{
	struct uring_bpf_ops *ops = kdata;

	io_bpf_reg_unreg(ops, false);
}

static int io_bpf_prep_io(struct uring_bpf_data *data, const struct io_uring_sqe *sqe)
{
	return -EOPNOTSUPP;
}

static int io_bpf_issue_io(struct uring_bpf_data *data)
{
	return -ECANCELED;
}

static void io_bpf_fail_io(struct uring_bpf_data *data)
{
}

static void io_bpf_cleanup_io(struct uring_bpf_data *data)
{
}

static struct uring_bpf_ops __bpf_uring_bpf_ops = {
	.prep_fn	= io_bpf_prep_io,
	.issue_fn	= io_bpf_issue_io,
	.fail_fn	= io_bpf_fail_io,
	.cleanup_fn	= io_bpf_cleanup_io,
};

static struct bpf_struct_ops bpf_uring_bpf_ops = {
	.verifier_ops = &io_bpf_verifier_ops,
	.init = uring_bpf_ops_init,
	.check_member = uring_bpf_ops_check_member,
	.init_member = uring_bpf_ops_init_member,
	.reg = io_bpf_reg,
	.unreg = io_bpf_unreg,
	.name = "uring_bpf_ops",
	.cfi_stubs = &__bpf_uring_bpf_ops,
	.owner = THIS_MODULE,
};

/*
 * Helper to copy data between two iov_iters using page extraction.
 * Extracts pages from source iterator and copies them to destination.
 * Returns number of bytes copied or negative error code.
 */
static ssize_t io_bpf_copy_iters(struct iov_iter *src, struct iov_iter *dst,
				 size_t len)
{
#define MAX_PAGES_PER_LOOP 32
	struct page *pages[MAX_PAGES_PER_LOOP];
	size_t total_copied = 0;
	bool need_unpin;

	/* Determine if we'll need to unpin pages later */
	need_unpin = user_backed_iter(src);

	/* Process pages in chunks */
	while (len > 0) {
		struct page **page_array = pages;
		size_t offset, copied = 0;
		ssize_t extracted;
		unsigned int nr_pages;
		size_t chunk_len;
		int i;

		/* Extract up to MAX_PAGES_PER_LOOP pages */
		chunk_len = min_t(size_t, len, MAX_PAGES_PER_LOOP * PAGE_SIZE);
		extracted = iov_iter_extract_pages(src, &page_array, chunk_len,
						   MAX_PAGES_PER_LOOP, 0, &offset);
		if (extracted <= 0) {
			if (total_copied > 0)
				break;
			return extracted < 0 ? extracted : -EFAULT;
		}

		nr_pages = DIV_ROUND_UP(offset + extracted, PAGE_SIZE);

		/* Copy pages to destination iterator */
		for (i = 0; i < nr_pages && copied < extracted; i++) {
			size_t page_offset = (i == 0) ? offset : 0;
			size_t page_len = min_t(size_t, extracted - copied,
						PAGE_SIZE - page_offset);
			size_t n;

			n = copy_page_to_iter(pages[i], page_offset, page_len, dst);
			copied += n;
			if (n < page_len)
				break;
		}

		/* Clean up extracted pages */
		if (need_unpin)
			unpin_user_pages(pages, nr_pages);

		total_copied += copied;
		len -= copied;

		/* Stop if we didn't copy all extracted data */
		if (copied < extracted)
			break;
	}

	return total_copied;
#undef MAX_PAGES_PER_LOOP
}

/*
 * Helper to import a buffer into an iov_iter for BPF memcpy operations.
 * Handles both plain user buffers and fixed/registered buffers.
 *
 * @req: io_kiocb request
 * @iter: output iterator
 * @buf_type: buffer type (plain or fixed)
 * @addr: buffer address
 * @offset: offset into buffer
 * @len: length from offset
 * @direction: ITER_SOURCE for source buffer, ITER_DEST for destination
 * @issue_flags: io_uring issue flags
 *
 * Returns 0 on success, negative error code on failure.
 */
static int io_bpf_import_buffer(struct io_kiocb *req, struct iov_iter *iter,
				u8 buf_type, u64 addr, unsigned int offset,
				u32 len, int direction, unsigned int issue_flags)
{
	if (buf_type == IORING_BPF_BUF_TYPE_PLAIN) {
		/* Plain user buffer */
		return import_ubuf(direction, (void __user *)(addr + offset),
				   len - offset, iter);
	} else if (buf_type == IORING_BPF_BUF_TYPE_FIXED) {
		/* Fixed buffer */
		return io_import_reg_buf(req, iter, addr + offset,
					 len - offset, direction, issue_flags);
	}

	return -EINVAL;
}

__bpf_kfunc_start_defs();
__bpf_kfunc void uring_bpf_set_result(struct uring_bpf_data *data, int res)
{
	struct io_kiocb *req = cmd_to_io_kiocb(data);

	if (res < 0)
		req_set_fail(req);
	io_req_set_res(req, res, 0);
}

/* io_kiocb layout might be changed */
__bpf_kfunc struct io_kiocb *uring_bpf_data_to_req(struct uring_bpf_data *data)
{
	return cmd_to_io_kiocb(data);
}

/**
 * io_uring_bpf_req_memcpy - Copy data between io_uring BPF request buffers
 * @data: BPF request data containing buffer metadata
 * @dest: Destination buffer descriptor (with buf_id and offset)
 * @src: Source buffer descriptor (with buf_id and offset)
 * @len: Number of bytes to copy
 *
 * Copies data between two different io_uring BPF request buffers (buf_id 1 and 2).
 * Supports: plain-to-plain, fixed-to-plain, and plain-to-fixed.
 * Does not support copying within the same buffer (src and dest must be different).
 *
 * Returns: Number of bytes copied on success, negative error code on failure
 */
__bpf_kfunc int io_uring_bpf_req_memcpy(struct uring_bpf_data *data,
					struct bpf_req_mem_desc *dest,
					struct bpf_req_mem_desc *src,
					unsigned int len)
{
	struct io_kiocb *req = cmd_to_io_kiocb(data);
	struct iov_iter dst_iter, src_iter;
	u8 dst_type, src_type;
	u64 dst_addr, src_addr;
	u32 dst_len, src_len;
	int ret;

	/* Validate buffer IDs */
	if (dest->buf_id < 1 || dest->buf_id > 2 ||
	    src->buf_id < 1 || src->buf_id > 2)
		return -EINVAL;

	/* Don't allow copying within the same buffer */
	if (src->buf_id == dest->buf_id)
		return -EINVAL;

	/* Extract source buffer metadata */
	if (src->buf_id == 1) {
		src_type = IORING_BPF_BUF1_TYPE(data->opf);
		src_addr = data->buf1_addr;
		src_len = data->buf1_len;
	} else {
		src_type = IORING_BPF_BUF2_TYPE(data->opf);
		src_addr = data->buf2_addr;
		src_len = data->buf2_len;
	}

	/* Extract destination buffer metadata */
	if (dest->buf_id == 1) {
		dst_type = IORING_BPF_BUF1_TYPE(data->opf);
		dst_addr = data->buf1_addr;
		dst_len = data->buf1_len;
	} else {
		dst_type = IORING_BPF_BUF2_TYPE(data->opf);
		dst_addr = data->buf2_addr;
		dst_len = data->buf2_len;
	}

	/* Validate offsets and lengths */
	if (src->offset + len > src_len || dest->offset + len > dst_len)
		return -EINVAL;

	/* Initialize source iterator */
	ret = io_bpf_import_buffer(req, &src_iter, src_type,
				   src_addr, src->offset, src_len,
				   ITER_SOURCE, data->issue_flags);
	if (ret)
		return ret;

	/* Initialize destination iterator */
	ret = io_bpf_import_buffer(req, &dst_iter, dst_type,
				   dst_addr, dest->offset, dst_len,
				   ITER_DEST, data->issue_flags);
	if (ret)
		return ret;

	/* Extract pages from source iterator and copy to destination */
	return io_bpf_copy_iters(&src_iter, &dst_iter, len);
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(uring_bpf_kfuncs)
BTF_ID_FLAGS(func, uring_bpf_set_result)
BTF_ID_FLAGS(func, uring_bpf_data_to_req)
BTF_ID_FLAGS(func, io_uring_bpf_req_memcpy)
BTF_KFUNCS_END(uring_bpf_kfuncs)

static const struct btf_kfunc_id_set uring_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &uring_bpf_kfuncs,
};

int __init io_bpf_init(void)
{
	int err;

	err = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &uring_kfunc_set);
	if (err) {
		pr_warn("error while setting UBLK BPF tracing kfuncs: %d", err);
		return err;
	}

	err = register_bpf_struct_ops(&bpf_uring_bpf_ops, uring_bpf_ops);
	if (err)
		pr_warn("error while registering io_uring bpf struct ops: %d", err);

	return 0;
}
