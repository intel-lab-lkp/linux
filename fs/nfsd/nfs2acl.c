// SPDX-License-Identifier: GPL-2.0
/*
 * Process version 2 NFSACL requests.
 *
 * Copyright (C) 2002-2003 Andreas Gruenbacher <agruen@suse.de>
 */

#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/nfsacl.h>
#include <linux/nfs3.h>
#include <linux/nfs4.h>

#include <uapi/linux/nfsd/debug.h>

#include "nfs_acl2xdr_gen.h"

#include "cache.h"
#include "nfsd.h"
#include "auth.h"
#include "vfs.h"

#define NFSDDBG_FACILITY	NFSDDBG_PROC

#define NFS_ACL_MASK		(NA_ACL | NA_ACLCNT | NA_DFACL | NA_DFACLCNT)

/*
 * The generated <linux/sunrpc/xdrgen/nfs_acl2.h> defines enum ftype, so
 * <uapi/linux/nfs.h> (which also defines enum nfs_ftype with the same
 * NFNON..NFLNK enumerators) and the generated nfs2 header cannot be
 * included here. Define the inherited NFSv2 constant we need.
 */
#define NFS_MAXPATHLEN 1024

struct posix_acl;

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

struct SETACL2res_wrapper {
	struct SETACL2res	xdrgen;
	struct kstat		stat;
};

static_assert(offsetof(struct SETACL2res_wrapper, xdrgen) == 0);

struct GETATTR2args_wrapper {
	struct GETATTR2args	xdrgen;
	struct svc_fh		fh;
};

static_assert(offsetof(struct GETATTR2args_wrapper, xdrgen) == 0);

struct GETATTR2res_wrapper {
	struct GETATTR2res	xdrgen;
	struct kstat		stat;
};

static_assert(offsetof(struct GETATTR2res_wrapper, xdrgen) == 0);

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

static __always_inline void
nfsacld_fhandle_to_svc_fh(struct svc_fh *fhp, const fhandle *fhandle)
{
	fh_init(fhp, FHSIZE);
	fhp->fh_handle.fh_size = FHSIZE;
	memcpy(&fhp->fh_handle.fh_raw, fhandle, FHSIZE);
}

static __always_inline void
nfsacld_timespec64_to_timeval(struct timeval *dst,
			      const struct timespec64 *src)
{
	dst->seconds = src->tv_sec;
	dst->useconds = src->tv_nsec / NSEC_PER_USEC;
}

static u32
nfsacld_mode_to_ftype(umode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:  return NFREG;
	case S_IFDIR:  return NFDIR;
	case S_IFBLK:  return NFBLK;
	case S_IFCHR:  return NFCHR;
	case S_IFLNK:  return NFLNK;
	/*
	 * The NFSv2 protocol's ftype enum does not provide specific values
	 * for sockets and FIFOs.
	 */
	case S_IFSOCK: return NFNON;
	case S_IFIFO:  return NFNON;
	}
	return NFNON;
}

static void
nfsacld_stat_to_fattr(struct svc_rqst *rqstp, struct fattr *fattr,
		      const struct kstat *stat, const struct svc_fh *fhp)
{
	struct user_namespace *userns = nfsd_user_namespace(rqstp);
	struct timespec64 time;
	u32 fsid;

	fattr->type = nfsacld_mode_to_ftype(stat->mode);
	fattr->mode = stat->mode;
	fattr->nlink = stat->nlink;
	fattr->uid = from_kuid_munged(userns, stat->uid);
	fattr->gid = from_kgid_munged(userns, stat->gid);
	if (S_ISLNK(stat->mode) && stat->size > NFS_MAXPATHLEN)
		fattr->size = NFS_MAXPATHLEN;
	else
		fattr->size = stat->size;
	fattr->blocksize = stat->blksize;
	if (S_ISCHR(stat->mode) || S_ISBLK(stat->mode))
		fattr->rdev = new_encode_dev(stat->rdev);
	else
		fattr->rdev = 0xffffffff;
	fattr->blocks = stat->blocks;

	switch (fsid_source(fhp)) {
	case FSIDSOURCE_FSID:
		fsid = (u32)fhp->fh_export->ex_fsid;
		break;
	case FSIDSOURCE_UUID:
		fsid = ((u32 *)fhp->fh_export->ex_uuid)[0];
		fsid ^= ((u32 *)fhp->fh_export->ex_uuid)[1];
		fsid ^= ((u32 *)fhp->fh_export->ex_uuid)[2];
		fsid ^= ((u32 *)fhp->fh_export->ex_uuid)[3];
		break;
	default:
		fsid = new_encode_dev(stat->dev);
		break;
	}
	fattr->fsid = fsid;
	fattr->fileid = stat->ino;

	nfsacld_timespec64_to_timeval(&fattr->atime, &stat->atime);
	time = stat->mtime;
	lease_get_mtime(d_inode(fhp->fh_dentry), &time);
	nfsacld_timespec64_to_timeval(&fattr->mtime, &time);
	nfsacld_timespec64_to_timeval(&fattr->ctime, &stat->ctime);
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

/**
 * nfsacld_proc_getacl - GETACL: Get the Access and/or Default ACL of a file
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   GETACL2res ACLPROC2_GETACL(GETACL2args) = 1;
 */
static __be32 nfsacld_proc_getacl(struct svc_rqst *rqstp)
{
	struct GETACL2args_wrapper *argp = rqstp->rq_argp;
	struct GETACL2res_wrapper *resp = rqstp->rq_resp;
	struct svc_fh *fhp = &argp->fh;
	struct posix_acl *acl;
	struct inode *inode;

	nfsacld_fhandle_to_svc_fh(fhp, &argp->xdrgen.fh);
	dprintk("nfsd: GETACL(2acl)   %s\n", SVCFH_fmt(fhp));

	resp->xdrgen.status = fh_verify(rqstp, fhp, 0, NFSD_MAY_NOP);
	if (resp->xdrgen.status != acl2_ok)
		goto out;
	inode = d_inode(fhp->fh_dentry);

	if (argp->xdrgen.mask & ~NFS_ACL_MASK) {
		resp->xdrgen.status = acl2err_inval;
		goto out;
	}
	resp->mask = argp->xdrgen.mask;

	resp->xdrgen.status = fh_getattr(fhp, &resp->stat);
	if (resp->xdrgen.status != acl2_ok)
		goto out;

	nfsacld_stat_to_fattr(rqstp, &resp->xdrgen.u.resok.attr, &resp->stat,
			      fhp);

	if (resp->mask & (NA_ACL | NA_ACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
		if (acl == NULL) {
			/*
			 * When no ACL is present, emit a minimal ACL from
			 * the mode bits, per draft-ietf-nfsv4-nfs-acl
			 * Section 5.2.2.4.
			 */
			acl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);
		}
		if (IS_ERR(acl)) {
			resp->xdrgen.status = nfsacld_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_access = acl;
	}
	if (resp->mask & (NA_DFACL | NA_DFACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_DEFAULT);
		if (IS_ERR(acl)) {
			resp->xdrgen.status = nfsacld_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_default = acl;
	}

	/*
	 * The generated secattr encoder streams the ACL entries from
	 * the posix_acls above via the encode hooks; the proc supplies
	 * the scalar fields.  aclcnt/dfaclcnt carry the entry count
	 * independent of whether the matching list is requested.
	 */
	resp->xdrgen.u.resok.acl.mask = resp->mask;
	resp->xdrgen.u.resok.acl.aclcnt = nfsacld_acl_entries(resp->acl_access);
	resp->xdrgen.u.resok.acl.dfaclcnt = nfsacld_acl_entries(resp->acl_default);

out:
	/* resp->acl_{access,default} are released in nfsaclsvc_release_getacl. */
	fh_put(fhp);
	resp->xdrgen.status = nfsacld_map_status(resp->xdrgen.status);
	return rpc_success;

fail:
	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
	resp->acl_access = NULL;
	resp->acl_default = NULL;
	goto out;
}

/**
 * nfsacld_proc_setacl - SETACL: Set the Access and/or Default ACL of a file
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   SETACL2res ACLPROC2_SETACL(SETACL2args) = 2;
 */
static __be32 nfsacld_proc_setacl(struct svc_rqst *rqstp)
{
	struct SETACL2args_wrapper *argp = rqstp->rq_argp;
	struct SETACL2res_wrapper *resp = rqstp->rq_resp;
	struct svc_fh *fhp = &argp->fh;
	struct inode *inode;
	int error;

	nfsacld_fhandle_to_svc_fh(fhp, &argp->xdrgen.fh);
	dprintk("nfsd: SETACL(2acl)   %s\n", SVCFH_fmt(fhp));

	/* A well-formed but invalid ACL is rejected before authorization. */
	if (argp->invalid) {
		resp->xdrgen.status = acl2err_inval;
		goto out;
	}

	resp->xdrgen.status = fh_verify(rqstp, fhp, 0, NFSD_MAY_SATTR);
	if (resp->xdrgen.status != acl2_ok)
		goto out;
	inode = d_inode(fhp->fh_dentry);

	/* The VFS reports a default ACL on a non-directory as EACCES. */
	if (argp->acl_default && !S_ISDIR(inode->i_mode)) {
		resp->xdrgen.status = acl2err_inval;
		goto out;
	}

	error = fh_want_write(fhp);
	if (error)
		goto out_errno;

	inode_lock(inode);

	error = 0;
	if (argp->xdrgen.acl.mask & NA_ACL) {
		error = set_posix_acl(&nop_mnt_idmap, fhp->fh_dentry,
				      ACL_TYPE_ACCESS, argp->acl_access);
		if (error)
			goto out_drop_lock;
	}
	if (argp->xdrgen.acl.mask & NA_DFACL) {
		error = set_posix_acl(&nop_mnt_idmap, fhp->fh_dentry,
				      ACL_TYPE_DEFAULT, argp->acl_default);
		if (error)
			goto out_drop_lock;
	}

	inode_unlock(inode);

	fh_drop_write(fhp);

	resp->xdrgen.status = fh_getattr(fhp, &resp->stat);

out:
	if (resp->xdrgen.status == acl2_ok)
		nfsacld_stat_to_fattr(rqstp, &resp->xdrgen.u.resok.attr,
				      &resp->stat, fhp);
	/* argp->acl_{access,default} are released in nfsaclsvc_release_setacl. */
	fh_put(fhp);
	resp->xdrgen.status = nfsacld_map_status(resp->xdrgen.status);
	return rpc_success;

out_drop_lock:
	inode_unlock(inode);
	fh_drop_write(fhp);
out_errno:
	resp->xdrgen.status = nfsacld_map_errno(error);
	goto out;
}

/**
 * nfsacld_proc_getattr - GETATTR: Get the attributes of a file
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   GETATTR2res ACLPROC2_GETATTR(GETATTR2args) = 3;
 */
static __be32 nfsacld_proc_getattr(struct svc_rqst *rqstp)
{
	struct GETATTR2args_wrapper *argp = rqstp->rq_argp;
	struct GETATTR2res_wrapper *resp = rqstp->rq_resp;
	struct svc_fh *fhp = &argp->fh;

	nfsacld_fhandle_to_svc_fh(fhp, &argp->xdrgen.fh);
	dprintk("nfsd: GETATTR  %s\n", SVCFH_fmt(fhp));

	resp->xdrgen.status = fh_verify(rqstp, fhp, 0, NFSD_MAY_NOP);
	if (resp->xdrgen.status != acl2_ok)
		goto out;
	resp->xdrgen.status = fh_getattr(fhp, &resp->stat);

out:
	if (resp->xdrgen.status == acl2_ok)
		nfsacld_stat_to_fattr(rqstp, &resp->xdrgen.u.resok.attr,
				      &resp->stat, fhp);
	fh_put(fhp);
	resp->xdrgen.status = nfsacld_map_status(resp->xdrgen.status);
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
	struct GETACL2res_wrapper *resp = rqstp->rq_resp;

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
	struct SETACL2args_wrapper *argp = rqstp->rq_argp;

	posix_acl_release(argp->acl_access);
	posix_acl_release(argp->acl_default);
}

#define ST 1		/* status*/
#define AT 21		/* attributes */

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
		.pc_func	= nfsacld_proc_getacl,
		.pc_decode	= nfs_acl_svc_decode_GETACL2args,
		.pc_encode	= nfs_acl_svc_encode_GETACL2res,
		.pc_release	= nfsaclsvc_release_getacl,
		.pc_argsize	= sizeof(struct GETACL2args_wrapper),
		.pc_argzero	= 0,
		.pc_ressize	= sizeof(struct GETACL2res_wrapper),
		.pc_cachetype	= RC_NOCACHE,
		.pc_xdrressize	= NFS_ACL2_GETACL2res_sz,
		.pc_name	= "GETACL",
	},
	[ACLPROC2_SETACL] = {
		.pc_func	= nfsacld_proc_setacl,
		.pc_decode	= nfs_acl_svc_decode_SETACL2args,
		.pc_encode	= nfs_acl_svc_encode_SETACL2res,
		.pc_release	= nfsaclsvc_release_setacl,
		.pc_argsize	= sizeof(struct SETACL2args_wrapper),
		.pc_argzero	= sizeof(struct SETACL2args_wrapper),
		.pc_ressize	= sizeof(struct SETACL2res_wrapper),
		.pc_cachetype	= RC_NOCACHE,
		.pc_xdrressize	= NFS_ACL2_SETACL2res_sz,
		.pc_name	= "SETACL",
	},
	[ACLPROC2_GETATTR] = {
		.pc_func	= nfsacld_proc_getattr,
		.pc_decode	= nfs_acl_svc_decode_GETATTR2args,
		.pc_encode	= nfs_acl_svc_encode_GETATTR2res,
		.pc_argsize	= sizeof(struct GETATTR2args_wrapper),
		.pc_argzero	= 0,
		.pc_ressize	= sizeof(struct GETATTR2res_wrapper),
		.pc_cachetype	= RC_NOCACHE,
		.pc_xdrressize	= NFS_ACL2_GETATTR2res_sz,
		.pc_name	= "GETATTR",
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
	struct GETACL2args_wrapper	getaclargs;
	struct SETACL2args_wrapper	setaclargs;
	struct GETATTR2args_wrapper	getattrargs;
	struct nfsd3_accessargs		accessargs;
	struct GETACL2res_wrapper	getaclres;
	struct SETACL2res_wrapper	setaclres;
	struct GETATTR2res_wrapper	getattrres;
	struct nfsd3_accessres		accessres;
};

const struct svc_version nfsd_acl_version2 = {
	.vs_vers	= 2,
	.vs_nproc	= ARRAY_SIZE(nfsd_acl_procedures2),
	.vs_proc	= nfsd_acl_procedures2,
	.vs_dispatch	= nfsd_dispatch,
	.vs_xdrsize	= sizeof(union nfsd_acl2_xdrstore),
};
