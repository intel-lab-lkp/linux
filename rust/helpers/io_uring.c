// SPDX-License-Identifier: GPL-2.0

#include <linux/io_uring/cmd.h>

__rust_helper void rust_helper_io_uring_cmd_done32(struct io_uring_cmd *cmd, s32 ret,
						    u64 res2, unsigned int issue_flags)
{
	io_uring_cmd_done32(cmd, ret, res2, issue_flags);
}
