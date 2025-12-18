/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_IO_URING_BUF_H
#define _LINUX_IO_URING_BUF_H

#include <linux/io_uring_types.h>

#if defined(CONFIG_IO_URING)
struct io_br_sel io_ring_buffer_select(struct io_kiocb *req, size_t *len,
				       struct io_buffer_list *bl,
				       unsigned int issue_flags);
#else
static inline struct io_br_sel io_ring_buffer_select(struct io_kiocb *req,
						     size_t *len,
						     struct io_buffer_list *bl,
						     unsigned int issue_flags)
{
	struct io_br_sel sel = {
		.val = -EOPNOTSUPP,
	};

	return sel;
}
#endif /* CONFIG_IO_URING */

#endif /* _LINUX_IO_URING_BUF_H */
