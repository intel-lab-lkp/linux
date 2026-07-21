// SPDX-License-Identifier: GPL-2.0

void io_xattr_cleanup(struct io_kiocb *req);

int io_fsetxattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_fsetxattr(struct io_kiocb *req, unsigned int issue_flags);

int io_setxattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_setxattr(struct io_kiocb *req, unsigned int issue_flags);

int io_fgetxattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_fgetxattr(struct io_kiocb *req, unsigned int issue_flags);

int io_getxattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_getxattr(struct io_kiocb *req, unsigned int issue_flags);

int io_fremovexattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_fremovexattr(struct io_kiocb *req, unsigned int issue_flags);

int io_flistxattr_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_flistxattr(struct io_kiocb *req, unsigned int issue_flags);
