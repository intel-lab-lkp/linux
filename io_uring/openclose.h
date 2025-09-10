// SPDX-License-Identifier: GPL-2.0

int __io_close_fixed(struct io_ring_ctx *ctx, unsigned int issue_flags,
		     unsigned int offset);

int io_openat_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_openat(struct io_kiocb *req, unsigned int issue_flags);
void io_open_cleanup(struct io_kiocb *req);

int io_openat2_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_openat2(struct io_kiocb *req, unsigned int issue_flags);

struct io_open_handle_async {
	struct file_handle		*handle;
	struct path			path;
};

#if defined(CONFIG_FHANDLE)
int io_name_to_handle_at_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_name_to_handle_at(struct io_kiocb *req, unsigned int issue_flags);
int io_open_by_handle_at_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_open_by_handle_at(struct io_kiocb *req, unsigned int issue_flags);
void io_open_by_handle_cleanup(struct io_kiocb *req);
#endif /* CONFIG_FHANDLE */

int io_close_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_close(struct io_kiocb *req, unsigned int issue_flags);

int io_pipe_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_pipe(struct io_kiocb *req, unsigned int issue_flags);

int io_install_fixed_fd_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_install_fixed_fd(struct io_kiocb *req, unsigned int issue_flags);
