// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/nfs_fs.h>
#include <linux/posix_acl.h>

#include "nfs.h"

/*
 * nfs34_prepare_get_acl, nfs34_complete_get_acl, nfs34_abort_get_acl: Helpers
 * for caching get_acl results in a race-free way. See fs/posix_acl.c:get_acl()
 * for explanations.
 */
void nfs34_prepare_get_acl(struct posix_acl **p)
{
	struct posix_acl *sentinel = uncached_acl_sentinel(current);

	/* If the ACL isn't being read yet, set our sentinel. */
	cmpxchg(p, ACL_NOT_CACHED, sentinel);
}
EXPORT_SYMBOL_GPL(nfs34_prepare_get_acl);

void nfs34_complete_get_acl(struct posix_acl **p, struct posix_acl *acl)
{
	struct posix_acl *sentinel = uncached_acl_sentinel(current);

	/* Only cache the ACL if our sentinel is still in place. */
	posix_acl_dup(acl);
	if (cmpxchg(p, sentinel, acl) != sentinel)
		posix_acl_release(acl);
}
EXPORT_SYMBOL_GPL(nfs34_complete_get_acl);

void nfs34_abort_get_acl(struct posix_acl **p)
{
	struct posix_acl *sentinel = uncached_acl_sentinel(current);

	/* Remove our sentinel upon failure. */
	cmpxchg(p, sentinel, ACL_NOT_CACHED);
}
EXPORT_SYMBOL_GPL(nfs34_abort_get_acl);
