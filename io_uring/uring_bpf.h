// SPDX-License-Identifier: GPL-2.0
#ifndef IOU_BPF_H
#define IOU_BPF_H

struct uring_bpf_data {
	/* readonly for bpf prog */
	struct file     *file;
	u32		opf;

	/* writeable for bpf prog */
	u8              pdu[64 - sizeof(struct file *) - sizeof(u32)];
};

typedef int (*uring_io_prep_t)(struct uring_bpf_data *data,
			       const struct io_uring_sqe *sqe);
typedef int (*uring_io_issue_t)(struct uring_bpf_data *data);
typedef void (*uring_io_fail_t)(struct uring_bpf_data *data);
typedef void (*uring_io_cleanup_t)(struct uring_bpf_data *data);

struct uring_bpf_ops {
	unsigned short		id;
	uring_io_prep_t		prep_fn;
	uring_io_issue_t	issue_fn;
	uring_io_fail_t		fail_fn;
	uring_io_cleanup_t	cleanup_fn;
};

#ifdef CONFIG_IO_URING_BPF
int io_uring_bpf_issue(struct io_kiocb *req, unsigned int issue_flags);
int io_uring_bpf_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
void io_uring_bpf_fail(struct io_kiocb *req);
void io_uring_bpf_cleanup(struct io_kiocb *req);

void uring_bpf_add_ctx(struct io_ring_ctx *ctx);
void uring_bpf_del_ctx(struct io_ring_ctx *ctx);

int __init io_bpf_init(void);

#else
static inline int io_uring_bpf_issue(struct io_kiocb *req, unsigned int issue_flags)
{
	return -ECANCELED;
}
static inline int io_uring_bpf_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	return -EOPNOTSUPP;
}
static inline void io_uring_bpf_fail(struct io_kiocb *req)
{
}
static inline void io_uring_bpf_cleanup(struct io_kiocb *req)
{
}

static inline void uring_bpf_add_ctx(struct io_ring_ctx *ctx)
{
}
static inline void uring_bpf_del_ctx(struct io_ring_ctx *ctx)
{
}

static inline int __init io_bpf_init(void)
{
	return 0;
}
#endif
#endif
