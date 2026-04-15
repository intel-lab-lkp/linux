// SPDX-License-Identifier: GPL-2.0

#include <linux/io_uring/cmd.h>

__rust_helper void rust_helper_io_uring_cmd_done32(struct io_uring_cmd *cmd, s32 ret,
						    u64 res2, unsigned int issue_flags)
{
	io_uring_cmd_done32(cmd, ret, res2, issue_flags);
}

__rust_helper struct io_uring_cmd *
rust_helper_io_uring_cmd_from_tw(struct io_tw_req tw_req)
{
	return io_uring_cmd_from_tw(tw_req);
}
