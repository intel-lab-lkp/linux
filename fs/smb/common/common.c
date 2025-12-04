// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) International Business Machines  Corp., 2009
 *                 2018 Samsung Electronics Co., Ltd.
 *   Author(s): Steve French <sfrench@us.ibm.com>
 *              Namjae Jeon <linkinjeon@kernel.org>
 */

#include <linux/module.h>
#include "common.h"

static int __init smb_common_init(void)
{
	int rc = 0;

	smb2_init_maperror();

	return rc;
}

static void __exit smb_common_exit(void)
{
}

MODULE_AUTHOR("Steve French <stfrench@microsoft.com>");
MODULE_AUTHOR("Namjae Jeon <linkinjeon@kernel.org>");
MODULE_DESCRIPTION("Linux kernel SMB common");
MODULE_LICENSE("GPL");
module_init(smb_common_init)
module_exit(smb_common_exit)
