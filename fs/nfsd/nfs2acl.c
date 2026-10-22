// SPDX-License-Identifier: GPL-2.0
/*
 * Process version 2 NFSACL requests.
 *
 * Copyright (C) 2002-2003 Andreas Gruenbacher <agruen@suse.de>
 */

#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/nfsacl.h>
#include <linux/nfs3.h>
#include <linux/nfs4.h>

#include <uapi/linux/nfsd/debug.h>

#include "nfs_acl2xdr_gen.h"

#include "cache.h"
#include "nfsd.h"
#include "vfs.h"

#define NFSDDBG_FACILITY	NFSDDBG_PROC

#define NFS_ACL_MASK		(NA_ACL | NA_ACLCNT | NA_DFACL | NA_DFACLCNT)

struct posix_acl;

struct nfsd_attrstat {
	__be32			status;
	struct svc_fh		fh;
	struct kstat		stat;
};

/*
 * Wrapper structures combine xdrgen types with legacy structures.
 * The xdrgen field must be first so the structure can be cast
 * to its XDR type for the RPC dispatch layer.
 */

struct GETACL2args_wrapper {
	struct GETACL2args	xdrgen;
	struct svc_fh		fh;
};

static_assert(offsetof(struct GETACL2args_wrapper, xdrgen) == 0);

struct GETACL2res_wrapper {
	struct GETACL2res	xdrgen;
	int			mask;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
	struct kstat		stat;
};

static_assert(offsetof(struct GETACL2res_wrapper, xdrgen) == 0);

struct SETACL2args_wrapper {
	struct SETACL2args	xdrgen;
	struct svc_fh		fh;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
	/*
	 * Set when the wire ACL is well-formed XDR but semantically
	 * invalid. The decoder accepts the request so that
	 * nfsacld_proc_setacl can return ACL2ERR_INVAL rather than RPC
	 * GARBAGE_ARGS. See draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
	 */
	bool			invalid;
};

static_assert(offsetof(struct SETACL2args_wrapper, xdrgen) == 0);

struct nfsd3_getaclargs {
	struct svc_fh		fh;
	__u32			mask;
};

struct nfsd3_getaclres {
	__be32			status;
	struct svc_fh		fh;
	int			mask;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
	struct kstat		stat;
};

struct nfsd3_setaclargs {
	struct svc_fh		fh;
	__u32			mask;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
};

struct nfsd3_accessargs {
	struct svc_fh		fh;
	__u32			access;
};

struct nfsd3_accessres {
	__be32			status;
	struct svc_fh		fh;
	__u32			access;
	struct kstat		stat;
};

bool nfssvc_decode_fhandleargs(struct svc_rqst *rqstp, struct xdr_stream *xdr);
bool nfssvc_encode_attrstatres(struct svc_rqst *rqstp, struct xdr_stream *xdr);

void nfssvc_release_attrstat(struct svc_rqst *rqstp);

bool svcxdr_decode_fhandle(struct xdr_stream *xdr, struct svc_fh *fhp);
bool svcxdr_encode_stat(struct xdr_stream *xdr, __be32 status);
bool svcxdr_encode_fattr(struct svc_rqst *rqstp, struct xdr_stream *xdr,
			 const struct svc_fh *fhp, const struct kstat *stat);

/*
 * These maps are identical to the NFSv3 maps (nfs3proc.c). This enables
 * the behavior of the two versions to diverge if needed.
 */
static const struct nfsd_access_map nfsd2_regaccess[] = {
	{ ACCESS2_READ,		NFSD_MAY_READ				},
	{ ACCESS2_EXECUTE,	NFSD_MAY_EXEC				},
	{ ACCESS2_MODIFY,	NFSD_MAY_WRITE | NFSD_MAY_TRUNC		},
	{ ACCESS2_EXTEND,	NFSD_MAY_WRITE				},
	{ 0,			0					}
};

static const struct nfsd_access_map nfsd2_diraccess[] = {
	{ ACCESS2_READ,		NFSD_MAY_READ				},
	{ ACCESS2_LOOKUP,	NFSD_MAY_EXEC				},
	{ ACCESS2_MODIFY,	NFSD_MAY_EXEC | NFSD_MAY_WRITE | NFSD_MAY_TRUNC },
	{ ACCESS2_EXTEND,	NFSD_MAY_EXEC | NFSD_MAY_WRITE		},
	{ ACCESS2_DELETE,	NFSD_MAY_REMOVE				},
	{ 0,			0					}
};

/*
 * Some clients - Solaris 2.6 at least, make an access call to the NFS
 * server to check for access for things like /dev/null (which really,
 * NFSD doesn't care about).  So NFSD provides simple access checking
 * for those objects, looking mainly at mode bits, ignoring read-only
 * filesystem checks.
 */
static const struct nfsd_access_map nfsd2_otheraccess[] = {
	{ ACCESS2_READ,		NFSD_MAY_READ				},
	{ ACCESS2_EXECUTE,	NFSD_MAY_EXEC				},
	{ ACCESS2_MODIFY,	NFSD_MAY_WRITE | NFSD_MAY_LOCAL_ACCESS	},
	{ ACCESS2_EXTEND,	NFSD_MAY_WRITE | NFSD_MAY_LOCAL_ACCESS	},
	{ 0,			0					}
};

static const struct nfsd_access_maps nfsd2_access_maps = {
	.regular	= nfsd2_regaccess,
	.directory	= nfsd2_diraccess,
	.other		= nfsd2_otheraccess,
};

enum {
	acl2_ok = __constant_cpu_to_be32(ACL2_OK),
	acl2err_perm = __constant_cpu_to_be32(ACL2ERR_PERM),
	acl2err_noent = __constant_cpu_to_be32(ACL2ERR_NOENT),
	acl2err_io = __constant_cpu_to_be32(ACL2ERR_IO),
	acl2err_acces = __constant_cpu_to_be32(ACL2ERR_ACCES),
	acl2err_inval = __constant_cpu_to_be32(ACL2ERR_INVAL),
	acl2err_nospc = __constant_cpu_to_be32(ACL2ERR_NOSPC),
	acl2err_rofs = __constant_cpu_to_be32(ACL2ERR_ROFS),
	acl2err_notsupp = __constant_cpu_to_be32(ACL2ERR_NOTSUPP),
	acl2err_dquot = __constant_cpu_to_be32(ACL2ERR_DQUOT),
	acl2err_stale = __constant_cpu_to_be32(ACL2ERR_STALE),
};

/* Ensure errno values do not leak onto the wire. */
static __be32 nfsacld_map_errno(int errno)
{
	switch (errno) {
	case 0:			return acl2_ok;
	case -EPERM:		return acl2err_perm;
	case -ENOENT:		return acl2err_noent;
	case -EACCES:		return acl2err_acces;
	case -EINVAL:		return acl2err_inval;
	case -ENOSPC:		return acl2err_nospc;
	case -EROFS:		return acl2err_rofs;
	case -EOPNOTSUPP:	return acl2err_notsupp;
	case -EDQUOT:		return acl2err_dquot;
	case -EBADF:
	case -EOPENSTALE:
	case -ESTALE:		return acl2err_stale;
	default:		return acl2err_io;
	}
}

static __be32 nfsacld_map_status(__be32 status)
{
	switch (status) {
	case cpu_to_be32(NFS4ERR_NOFILEHANDLE):
	case cpu_to_be32(NFS3ERR_BADHANDLE):
		status = acl2err_stale;
		break;
	case cpu_to_be32(NFS4ERR_WRONGSEC):
		status = acl2err_acces;
		break;
	}
	return status;
}

/*
 * A minimal (three-entry) ACL travels as four wire entries: the
 * POSIX entries plus a synthesized NA_CLASS_OBJ mask. The wire
 * element count is thus max(a_count, 4) when the ACL has at least
 * three entries, and zero otherwise. An ACL with fewer than three
 * entries has no minimal set to expand, so it travels as empty.
 * See draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
 */
static u32 nfsacld_acl_entries(const struct posix_acl *acl)
{
	return (acl && acl->a_count >= 3) ? max_t(int, acl->a_count, 4) : 0;
}

/**
 * nfsacld_proc_null - NULL: Do nothing
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   void ACLPROC2_NULL(void) = 0;
 */
static __be32 nfsacld_proc_null(struct svc_rqst *rqstp)
{
	return rpc_success;
}

/*
 * Get the Access and/or Default ACL of a file.
 */
static __be32 nfsacld_proc_getacl(struct svc_rqst *rqstp)
{
	struct nfsd3_getaclargs *argp = rqstp->rq_argp;
	struct nfsd3_getaclres *resp = rqstp->rq_resp;
	struct posix_acl *acl;
	struct inode *inode;
	svc_fh *fh;

	dprintk("nfsd: GETACL(2acl)   %s\n", SVCFH_fmt(&argp->fh));

	fh = fh_copy(&resp->fh, &argp->fh);
	resp->status = fh_verify(rqstp, &resp->fh, 0, NFSD_MAY_NOP);
	if (resp->status != acl2_ok)
		goto out;

	inode = d_inode(fh->fh_dentry);

	if (argp->mask & ~NFS_ACL_MASK) {
		resp->status = acl2err_io;
		goto out;
	}
	resp->mask = argp->mask;

	resp->status = fh_getattr(fh, &resp->stat);
	if (resp->status != acl2_ok)
		goto out;

	if (resp->mask & (NA_ACL | NA_ACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
		if (acl == NULL) {
			/* Solaris returns the inode's minimum ACL. */
			acl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);
		}
		if (IS_ERR(acl)) {
			resp->status = nfsacld_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_access = acl;
	}
	if (resp->mask & (NA_DFACL | NA_DFACLCNT)) {
		/* Check how Solaris handles requests for the Default ACL
		   of a non-directory! */
		acl = get_inode_acl(inode, ACL_TYPE_DEFAULT);
		if (IS_ERR(acl)) {
			resp->status = nfsacld_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_default = acl;
	}

	/* resp->acl_{access,default} are released in nfssvc_release_getacl. */
out:
	resp->status = nfsacld_map_status(resp->status);
	return rpc_success;

fail:
	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
	resp->acl_access = NULL;
	resp->acl_default = NULL;
	goto out;
}

/*
 * Set the Access and/or Default ACL of a file.
 */
static __be32 nfsacld_proc_setacl(struct svc_rqst *rqstp)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;
	struct nfsd_attrstat *resp = rqstp->rq_resp;
	struct inode *inode;
	svc_fh *fh;
	int error;

	dprintk("nfsd: SETACL(2acl)   %s\n", SVCFH_fmt(&argp->fh));

	fh = fh_copy(&resp->fh, &argp->fh);
	resp->status = fh_verify(rqstp, &resp->fh, 0, NFSD_MAY_SATTR);
	if (resp->status != acl2_ok)
		goto out;

	inode = d_inode(fh->fh_dentry);

	error = fh_want_write(fh);
	if (error)
		goto out_errno;

	inode_lock(inode);

	error = 0;
	if (argp->mask & NA_ACL) {
		error = set_posix_acl(&nop_mnt_idmap, fh->fh_dentry,
				      ACL_TYPE_ACCESS, argp->acl_access);
		if (error)
			goto out_drop_lock;
	}
	if (argp->mask & NA_DFACL) {
		error = set_posix_acl(&nop_mnt_idmap, fh->fh_dentry,
				      ACL_TYPE_DEFAULT, argp->acl_default);
		if (error)
			goto out_drop_lock;
	}

	inode_unlock(inode);

	fh_drop_write(fh);

	resp->status = fh_getattr(fh, &resp->stat);

out:
	/* argp->acl_{access,default} are released in nfsaclsvc_release_setacl. */
	resp->status = nfsacld_map_status(resp->status);
	return rpc_success;

out_drop_lock:
	inode_unlock(inode);
	fh_drop_write(fh);
out_errno:
	resp->status = nfsacld_map_errno(error);
	goto out;
}

/*
 * Check file attributes
 */
static __be32 nfsacld_proc_getattr(struct svc_rqst *rqstp)
{
	struct nfsd_fhandle *argp = rqstp->rq_argp;
	struct nfsd_attrstat *resp = rqstp->rq_resp;

	dprintk("nfsd: GETATTR  %s\n", SVCFH_fmt(&argp->fh));

	fh_copy(&resp->fh, &argp->fh);
	resp->status = fh_verify(rqstp, &resp->fh, 0, NFSD_MAY_NOP);
	if (resp->status != acl2_ok)
		goto out;
	resp->status = fh_getattr(&resp->fh, &resp->stat);
out:
	resp->status = nfsacld_map_status(resp->status);
	return rpc_success;
}

/*
 * Check file access
 */
static __be32 nfsacld_proc_access(struct svc_rqst *rqstp)
{
	struct nfsd3_accessargs *argp = rqstp->rq_argp;
	struct nfsd3_accessres *resp = rqstp->rq_resp;

	dprintk("nfsd: ACCESS(2acl)   %s 0x%x\n",
			SVCFH_fmt(&argp->fh),
			argp->access);

	fh_copy(&resp->fh, &argp->fh);
	resp->access = argp->access;

	resp->status = nfsd_access(rqstp, &resp->fh, &nfsd2_access_maps,
				   &resp->access, NULL);
	if (resp->status != acl2_ok)
		goto out;
	resp->status = fh_getattr(&resp->fh, &resp->stat);
out:
	resp->status = nfsacld_map_status(resp->status);
	return rpc_success;
}

/*
 * Aggregate codec hooks for the generated secattr codec.
 *
 * The generated codec owns the wire framing and drives these hooks
 * once per element; the hooks hold all posix_acl application logic. The
 * application context (@c->ctx) is the svc_rqst, bound to the stream by
 * svcxdr_init_{decode,encode}(); each hook reaches the proc's arg or
 * result wrapper through rq_{argp,resp}.
 * The cursor's member_id selects the member: it is
 * NFS_ACL2_SECATTR_ACLENT or NFS_ACL2_SECATTR_DFACLENT.
 */

/* GETACL reply: stream the wire entries out of the source posix_acl. */

bool nfs_acl2_secattr_encode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct GETACL2res_wrapper *resp = rqstp->rq_resp;
	bool dfacl = c->member_id == NFS_ACL2_SECATTR_DFACLENT;
	struct posix_acl *acl = dfacl ? resp->acl_default : resp->acl_access;
	int gate = dfacl ? (resp->mask & NA_DFACL) : (resp->mask & NA_ACL);

	c->count = gate ? nfsacld_acl_entries(acl) : 0;
	return true;
}

bool nfs_acl2_secattr_encode(struct xdrgen_aggregate_cursor *c,
			     struct aclent *out)
{
	struct svc_rqst *rqstp = c->ctx;
	struct GETACL2res_wrapper *resp = rqstp->rq_resp;
	bool dfacl = c->member_id == NFS_ACL2_SECATTR_DFACLENT;
	struct posix_acl *acl = dfacl ? resp->acl_default : resp->acl_access;
	int typeflag = dfacl ? NA_ACL_DEFAULT : 0;
	const struct posix_acl_entry *entry;
	int tag;

	/*
	 * A three-entry ACL expands to four wire entries in ascending
	 * type order (USER_OBJ, GROUP_OBJ, a NA_CLASS_OBJ mask cloned
	 * from GROUP_OBJ, OTHER). Some receivers depend on this ordering.
	 * See draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
	 */
	if (acl->a_count == 3) {
		static const u8 map[4] = { 0, 1, 1, 2 };

		entry = &acl->a_entries[map[c->index]];
		tag = (c->index == 2) ? ACL_MASK : entry->e_tag;
	} else {
		entry = &acl->a_entries[c->index];
		tag = entry->e_tag;
	}

	out->type = tag | typeflag;
	switch (tag) {
	case ACL_USER_OBJ:
		out->id = from_kuid(&init_user_ns, resp->stat.uid);
		break;
	case ACL_GROUP_OBJ:
		out->id = from_kgid(&init_user_ns, resp->stat.gid);
		break;
	case ACL_USER:
		out->id = from_kuid(&init_user_ns, entry->e_uid);
		break;
	case ACL_GROUP:
		out->id = from_kgid(&init_user_ns, entry->e_gid);
		break;
	default:
		/*
		 * The id field is undefined for these entry types;
		 * emit zero per draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
		 */
		out->id = 0;
		break;
	}
	out->perm = entry->e_perm & (ACL_READ | ACL_WRITE | ACL_EXECUTE);
	return true;
}

/*
 * The xdr_stream primitives do not advance rq_next_page, and svcrdma
 * retains only the pages below it for Send completion. The fields
 * that follow the entry list start a new page when this one is full.
 */
bool nfs_acl2_secattr_encode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct xdr_stream *xdr = c->xdr;

	rqstp->rq_next_page = xdr->page_ptr + 1;
	if (xdr->p == xdr->end)
		rqstp->rq_next_page++;
	return true;
}

/* SETACL args: consume the decoded wire entries into the target posix_acl. */

bool nfs_acl2_secattr_decode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL2args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL2_SECATTR_DFACLENT;
	struct posix_acl **pacl = dfacl ? &argp->acl_default : &argp->acl_access;
	u32 mask = argp->xdrgen.acl.mask;
	u32 declared = dfacl ? (u32)argp->xdrgen.acl.dfaclcnt :
			       (u32)argp->xdrgen.acl.aclcnt;
	int gate = dfacl ? (mask & NA_DFACL) : (mask & NA_ACL);

	/* Already rejected: consume the list. */
	if (argp->invalid)
		return true;
	/* The access list is decoded first, so the mask is checked once. */
	if (!dfacl && (mask & ~NFS_ACL_MASK)) {
		argp->invalid = true;
		return true;
	}
	/* Too many ACEs is an invalid ACL, not a malformed request. */
	if (declared > NFS_ACL_MAX_ENTRIES) {
		argp->invalid = true;
		return true;
	}

	if (gate) {
		/*
		 * A published list whose declared count differs from its
		 * length is an invalid ACL, not a malformed request.
		 */
		if (c->count != declared) {
			argp->invalid = true;
			return true;
		}
		if (!dfacl && !c->count) {
			argp->invalid = true;
			return true;
		}
		/* An empty default list publishes NULL: default ACL removal. */
		if (c->count) {
			*pacl = posix_acl_alloc(c->count, GFP_KERNEL);
			if (!*pacl)
				return false;
		}
	} else if (c->count > declared) {
		argp->invalid = true;
		return true;
	}
	return true;
}

bool nfs_acl2_secattr_decode(struct xdrgen_aggregate_cursor *c,
			     const struct aclent *in)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL2args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL2_SECATTR_DFACLENT;
	struct posix_acl *acl = dfacl ? argp->acl_default : argp->acl_access;
	struct posix_acl_entry *entry;

	/* Already rejected, or list gated out by mask: consume it. */
	if (argp->invalid || !acl)
		return true;

	/* A perm bit outside rwx makes the ACL invalid. */
	if (in->perm & ~(ACL_READ | ACL_WRITE | ACL_EXECUTE)) {
		argp->invalid = true;
		return true;
	}

	entry = &acl->a_entries[c->index];
	switch (in->type & ~NA_ACL_DEFAULT) {
	case ACL_USER:
		/* An id with no kernel mapping makes the ACL invalid. */
		entry->e_uid = make_kuid(&init_user_ns, in->id);
		if (!uid_valid(entry->e_uid)) {
			argp->invalid = true;
			return true;
		}
		break;
	case ACL_GROUP:
		entry->e_gid = make_kgid(&init_user_ns, in->id);
		if (!gid_valid(entry->e_gid)) {
			argp->invalid = true;
			return true;
		}
		break;
	case ACL_USER_OBJ:
	case ACL_GROUP_OBJ:
	case ACL_OTHER:
	case ACL_MASK:
		break;
	default:
		/* An undefined entry type makes the ACL invalid. */
		argp->invalid = true;
		return true;
	}
	entry->e_tag = in->type & ~NA_ACL_DEFAULT;
	entry->e_perm = in->perm;

	return true;
}

bool nfs_acl2_secattr_decode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL2args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL2_SECATTR_DFACLENT;
	struct posix_acl *acl = dfacl ? argp->acl_default : argp->acl_access;

	/*
	 * On failure the published ACL stays reachable through the arg
	 * wrapper and is freed by pc_release; nothing to do here.
	 */
	if (!ok || argp->invalid)
		return true;
	return posix_acl_from_nfsacl(acl) == 0;
}

/*
 * XDR decode functions
 */

static bool
nfsaclsvc_decode_getaclargs(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_getaclargs *argp = rqstp->rq_argp;

	if (!svcxdr_decode_fhandle(xdr, &argp->fh))
		return false;
	if (xdr_stream_decode_u32(xdr, &argp->mask) < 0)
		return false;

	return true;
}

static bool
nfsaclsvc_decode_setaclargs(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;

	if (!svcxdr_decode_fhandle(xdr, &argp->fh))
		return false;
	if (xdr_stream_decode_u32(xdr, &argp->mask) < 0)
		return false;
	if (argp->mask & ~NFS_ACL_MASK)
		return false;
	if (!nfs_stream_decode_acl(xdr, NULL, (argp->mask & NA_ACL) ?
				   &argp->acl_access : NULL))
		return false;
	if (!nfs_stream_decode_acl(xdr, NULL, (argp->mask & NA_DFACL) ?
				   &argp->acl_default : NULL))
		return false;

	return true;
}

static bool
nfsaclsvc_decode_accessargs(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_accessargs *args = rqstp->rq_argp;

	if (!svcxdr_decode_fhandle(xdr, &args->fh))
		return false;
	if (xdr_stream_decode_u32(xdr, &args->access) < 0)
		return false;

	return true;
}

/*
 * XDR encode functions
 */

/* GETACL */
static bool
nfsaclsvc_encode_getaclres(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_getaclres *resp = rqstp->rq_resp;
	struct dentry *dentry = resp->fh.fh_dentry;
	struct inode *inode;

	if (!svcxdr_encode_stat(xdr, resp->status))
		return false;
	switch (resp->status) {
	case acl2_ok:
		inode = d_inode(dentry);
		if (!svcxdr_encode_fattr(rqstp, xdr, &resp->fh, &resp->stat))
			return false;
		if (xdr_stream_encode_u32(xdr, resp->mask) < 0)
			return false;
		if (!nfs_stream_encode_acl(xdr, inode, resp->acl_access,
					   resp->mask & NA_ACL, 0))
			return false;
		if (!nfs_stream_encode_acl(xdr, inode, resp->acl_default,
					   resp->mask & NA_DFACL, NA_ACL_DEFAULT))
			return false;
		break;
	}

	return true;
}

/* ACCESS */
static bool
nfsaclsvc_encode_accessres(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_accessres *resp = rqstp->rq_resp;

	if (!svcxdr_encode_stat(xdr, resp->status))
		return false;
	switch (resp->status) {
	case acl2_ok:
		if (!svcxdr_encode_fattr(rqstp, xdr, &resp->fh, &resp->stat))
			return false;
		if (xdr_stream_encode_u32(xdr, resp->access) < 0)
			return false;
		break;
	}

	return true;
}

/*
 * XDR release functions
 */
static void nfsaclsvc_release_getacl(struct svc_rqst *rqstp)
{
	struct nfsd3_getaclres *resp = rqstp->rq_resp;

	fh_put(&resp->fh);
	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
}

static void nfsaclsvc_release_access(struct svc_rqst *rqstp)
{
	struct nfsd3_accessres *resp = rqstp->rq_resp;

	fh_put(&resp->fh);
}

static void nfsaclsvc_release_setacl(struct svc_rqst *rqstp)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;
	struct nfsd_attrstat *resp = rqstp->rq_resp;

	fh_put(&resp->fh);
	posix_acl_release(argp->acl_access);
	posix_acl_release(argp->acl_default);
}

#define ST 1		/* status*/
#define AT 21		/* attributes */
#define pAT (1+AT)	/* post attributes - conditional */
#define ACL (1+NFS_ACL_MAX_ENTRIES*3)  /* Access Control List */

static const struct svc_procedure nfsd_acl_procedures2[5] = {
	[ACLPROC2_NULL] = {
		.pc_func	= nfsacld_proc_null,
		.pc_decode	= xdrgen_svc_decode_void,
		.pc_encode	= xdrgen_svc_encode_void,
		.pc_argsize	= XDR_void,
		.pc_argzero	= 0,
		.pc_ressize	= 0,
		.pc_cachetype	= RC_NOCACHE,
		.pc_xdrressize	= XDR_void,
		.pc_name	= "NULL",
	},
	[ACLPROC2_GETACL] = {
		.pc_func = nfsacld_proc_getacl,
		.pc_decode = nfsaclsvc_decode_getaclargs,
		.pc_encode = nfsaclsvc_encode_getaclres,
		.pc_release = nfsaclsvc_release_getacl,
		.pc_argsize = sizeof(struct nfsd3_getaclargs),
		.pc_argzero = sizeof(struct nfsd3_getaclargs),
		.pc_ressize = sizeof(struct nfsd3_getaclres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+1+2*(1+ACL),
		.pc_name = "GETACL",
	},
	[ACLPROC2_SETACL] = {
		.pc_func = nfsacld_proc_setacl,
		.pc_decode = nfsaclsvc_decode_setaclargs,
		.pc_encode = nfssvc_encode_attrstatres,
		.pc_release = nfsaclsvc_release_setacl,
		.pc_argsize = sizeof(struct nfsd3_setaclargs),
		.pc_argzero = sizeof(struct nfsd3_setaclargs),
		.pc_ressize = sizeof(struct nfsd_attrstat),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+AT,
		.pc_name = "SETACL",
	},
	[ACLPROC2_GETATTR] = {
		.pc_func = nfsacld_proc_getattr,
		.pc_decode = nfssvc_decode_fhandleargs,
		.pc_encode = nfssvc_encode_attrstatres,
		.pc_release = nfssvc_release_attrstat,
		.pc_argsize = sizeof(struct nfsd_fhandle),
		.pc_argzero = sizeof(struct nfsd_fhandle),
		.pc_ressize = sizeof(struct nfsd_attrstat),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+AT,
		.pc_name = "GETATTR",
	},
	[ACLPROC2_ACCESS] = {
		.pc_func = nfsacld_proc_access,
		.pc_decode = nfsaclsvc_decode_accessargs,
		.pc_encode = nfsaclsvc_encode_accessres,
		.pc_release = nfsaclsvc_release_access,
		.pc_argsize = sizeof(struct nfsd3_accessargs),
		.pc_argzero = sizeof(struct nfsd3_accessargs),
		.pc_ressize = sizeof(struct nfsd3_accessres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+AT+1,
		.pc_name = "SETATTR",
	},
};

/*
 * Storage requirements for XDR arguments and results.
 */
union nfsd_acl2_xdrstore {
	struct nfsd3_getaclargs		getaclargs;
	struct nfsd3_setaclargs		setaclargs;
	struct nfsd3_accessargs		accessargs;
	struct nfsd_fhandle		fhandle;
	struct nfsd3_getaclres		getaclres;
	struct nfsd_attrstat		attrstat;
	struct nfsd3_accessres		accessres;
};

const struct svc_version nfsd_acl_version2 = {
	.vs_vers	= 2,
	.vs_nproc	= ARRAY_SIZE(nfsd_acl_procedures2),
	.vs_proc	= nfsd_acl_procedures2,
	.vs_dispatch	= nfsd_dispatch,
	.vs_xdrsize	= sizeof(union nfsd_acl2_xdrstore),
};
