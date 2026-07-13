/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, SUSE LLC
 *
 * Authors: Enzo Matsumiya <ematsumiya@suse.de>
 *
 * This file implements I/O compression support for SMB2 messages (SMB 3.1.1 only).
 * See compress/ for implementation details of each algorithm.
 *
 * References:
 * MS-SMB2 "3.1.4.4 Compressing the Message" - for compression details
 * MS-SMB2 "3.1.5.3 Decompressing the Chained Message" - for decompression details
 * MS-XCA - for details of the supported algorithms
 */
#ifndef _SMB_COMPRESS_H
#define _SMB_COMPRESS_H

#include <linux/uio.h>
#include <linux/kernel.h>
#include "cifsglob.h"

#ifdef CONFIG_CIFS_COMPRESSION
typedef int (*compress_send_fn)(struct TCP_Server_Info *, int, struct smb_rqst *);

int smb_compress(struct TCP_Server_Info *server, struct smb_rqst *rq,
		 compress_send_fn send_fn);
bool should_compress(const struct cifs_tcon *tcon, const struct smb_rqst *rq);
#else /* !CONFIG_CIFS_COMPRESSION */
static inline int smb_compress(void *unused1, void *unused2, void *unused3)
{
	return -EOPNOTSUPP;
}

static inline bool should_compress(void *unused1, void *unused2)
{
	return false;
}
#endif /* !CONFIG_CIFS_COMPRESSION */
#endif /* _SMB_COMPRESS_H */
