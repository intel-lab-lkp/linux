/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Mike Snitzer <snitzer@hammerspace.com>
 */
#ifndef __LINUX_NFSLOCALIO_H
#define __LINUX_NFSLOCALIO_H

#include <linux/list.h>
#include <linux/uuid.h>
#include <linux/nfs.h>

/*
 * Global list of nfsd_uuid_t instances, add/remove
 * is protected by fs/nfsd/nfssvc.c:nfsd_mutex.
 */
extern struct list_head nfsd_uuids;

/*
 * Each nfsd instance has an nfsd_uuid_t that is accessible through the
 * global nfsd_uuids list. Useful to allow a client to negotiate if localio
 * possible with its server.
 */
typedef struct {
	uuid_t uuid;
	struct list_head list;
} nfsd_uuid_t;

bool nfsd_uuid_is_local(const uuid_t *uuid);

typedef int (*nfs_to_nfsd_open_t)(struct rpc_clnt *, const struct cred *,
				  const struct nfs_fh *, const fmode_t,
				  struct file **);

nfs_to_nfsd_open_t get_nfsd_open_local_fh(void);
void put_nfsd_open_local_fh(void);

#endif  /* __LINUX_NFSLOCALIO_H */
