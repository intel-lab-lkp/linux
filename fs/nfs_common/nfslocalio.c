// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Mike Snitzer <snitzer@hammerspace.com>
 */

#include <linux/module.h>
#include <linux/rculist.h>
#include <linux/nfslocalio.h>

MODULE_LICENSE("GPL");

/*
 * Global list of nfsd_uuid_t instances, add/remove
 * is protected by fs/nfsd/nfssvc.c:nfsd_mutex.
 * Reads are protected RCU read lock (see below).
 */
LIST_HEAD(nfsd_uuids);
EXPORT_SYMBOL(nfsd_uuids);

/* Must be called with RCU read lock held. */
static const uuid_t * nfsd_uuid_lookup(const uuid_t *uuid)
{
	nfsd_uuid_t *nfsd_uuid;

	list_for_each_entry_rcu(nfsd_uuid, &nfsd_uuids, list)
		if (uuid_equal(&nfsd_uuid->uuid, uuid))
			return &nfsd_uuid->uuid;

	return &uuid_null;
}

bool nfsd_uuid_is_local(const uuid_t *uuid)
{
	const uuid_t *nfsd_uuid;

	rcu_read_lock();
	nfsd_uuid = nfsd_uuid_lookup(uuid);
	rcu_read_unlock();

	return !uuid_is_null(nfsd_uuid);
}
EXPORT_SYMBOL_GPL(nfsd_uuid_is_local);

/*
 * The nfs localio code needs to call into nfsd to do the filehandle -> struct path
 * mapping, but cannot be statically linked, because that will make the nfs module
 * depend on the nfsd module.
 *
 * Instead, do dynamic linking to the nfsd module (via nfs_common module). The
 * nfs_common module will only hold a reference on nfsd when localio is in use.
 * This allows some sanity checking, like giving up on localio if nfsd isn't loaded.
 */

extern int nfsd_open_local_fh(struct rpc_clnt *rpc_clnt,
			const struct cred *cred, const struct nfs_fh *nfs_fh,
			const fmode_t fmode, struct file **pfilp);

nfs_to_nfsd_open_t get_nfsd_open_local_fh(void)
{
	return symbol_request(nfsd_open_local_fh);
}
EXPORT_SYMBOL_GPL(get_nfsd_open_local_fh);

void put_nfsd_open_local_fh(void)
{
	symbol_put(nfsd_open_local_fh);
}
EXPORT_SYMBOL_GPL(put_nfsd_open_local_fh);
