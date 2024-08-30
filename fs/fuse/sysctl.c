// SPDX-License-Identifier: GPL-2.0
/*
* linux/fs/fuse/fuse_sysctl.c
*
* Sysctl interface to fuse parameters
*/
#include <linux/sysctl.h>

#include "fuse_i.h"

static struct ctl_table_header *fuse_table_header;

static struct ctl_table fuse_sysctl_table[] = {
	{
		.procname	= "default_request_timeout",
		.data		= &fuse_default_req_timeout,
		.maxlen		= sizeof(fuse_default_req_timeout),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
	{
		.procname	= "max_request_timeout",
		.data		= &fuse_max_req_timeout,
		.maxlen		= sizeof(fuse_max_req_timeout),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
};

int fuse_sysctl_register(void)
{
	fuse_table_header = register_sysctl("fs/fuse", fuse_sysctl_table);
	if (!fuse_table_header)
		return -ENOMEM;
	return 0;
}

void fuse_sysctl_unregister(void)
{
	unregister_sysctl_table(fuse_table_header);
	fuse_table_header = NULL;
}
