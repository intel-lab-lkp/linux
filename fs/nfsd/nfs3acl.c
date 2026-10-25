// SPDX-License-Identifier: GPL-2.0
/*
 * Process version 3 NFSACL requests.
 *
 * Copyright (C) 2002-2003 Andreas Gruenbacher <agruen@suse.de>
 */

#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/nfsacl.h>
#include <linux/nfs4.h>

#include "nfs_acl3xdr_gen.h"

#include "cache.h"
#include "nfsd.h"
#include "vfs.h"

#define NFS_ACL_MASK		(NA_ACL | NA_ACLCNT | NA_DFACL | NA_DFACLCNT)

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

struct nfsd3_attrstat {
	__be32			status;
	struct svc_fh		fh;
	struct kstat            stat;
};

/* Helper functions for NFSv3 ACL code */
bool svcxdr_decode_nfs_fh3(struct xdr_stream *xdr, struct svc_fh *fhp);
bool svcxdr_encode_nfsstat3(struct xdr_stream *xdr, __be32 status);
bool svcxdr_encode_post_op_attr(struct svc_rqst *rqstp, struct xdr_stream *xdr,
				const struct svc_fh *fhp);

enum {
	acl3_ok = __constant_cpu_to_be32(ACL3_OK),
	acl3err_perm = __constant_cpu_to_be32(ACL3ERR_PERM),
	acl3err_noent = __constant_cpu_to_be32(ACL3ERR_NOENT),
	acl3err_io = __constant_cpu_to_be32(ACL3ERR_IO),
	acl3err_acces = __constant_cpu_to_be32(ACL3ERR_ACCES),
	acl3err_inval = __constant_cpu_to_be32(ACL3ERR_INVAL),
	acl3err_nospc = __constant_cpu_to_be32(ACL3ERR_NOSPC),
	acl3err_rofs = __constant_cpu_to_be32(ACL3ERR_ROFS),
	acl3err_dquot = __constant_cpu_to_be32(ACL3ERR_DQUOT),
	acl3err_stale = __constant_cpu_to_be32(ACL3ERR_STALE),
	acl3err_badhandle = __constant_cpu_to_be32(ACL3ERR_BADHANDLE),
	acl3err_notsupp = __constant_cpu_to_be32(ACL3ERR_NOTSUPP),
	acl3err_serverfault = __constant_cpu_to_be32(ACL3ERR_SERVERFAULT),
	acl3err_jukebox = __constant_cpu_to_be32(ACL3ERR_JUKEBOX),
};

/* Ensure errno values do not leak onto the wire. */
static __be32 nfsd3_map_errno(int errno)
{
	switch (errno) {
	case 0:			return acl3_ok;
	case -EPERM:
	case -ENOKEY:		return acl3err_perm;
	case -ENOENT:		return acl3err_noent;
	case -EIO:
	case -EREMOTEIO:
	case -ETXTBSY:
	case -EUCLEAN:
	case -EBADMSG:		return acl3err_io;
	case -EACCES:		return acl3err_acces;
	case -EINVAL:		return acl3err_inval;
	case -ENOSPC:		return acl3err_nospc;
	case -EROFS:		return acl3err_rofs;
	case -EDQUOT:		return acl3err_dquot;
	case -EBADF:
	case -EOPENSTALE:
	case -ESTALE:		return acl3err_stale;
	case -EOPNOTSUPP:	return acl3err_notsupp;
	case -ENOMEM:
	case -EAGAIN:
	case -ERESTARTSYS:
	case -ETIMEDOUT:	return acl3err_jukebox;
	default:		return acl3err_serverfault;
	}
}

static __be32 nfsd3_map_status(__be32 status)
{
	switch (status) {
	case cpu_to_be32(NFS4ERR_NOFILEHANDLE):
		status = acl3err_badhandle;
		break;
	case cpu_to_be32(NFS4ERR_WRONGSEC):
		status = acl3err_acces;
		break;
	}
	return status;
}

/**
 * nfsd3_proc_null - NULL: Do nothing
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   void ACLPROC3_NULL(void) = 0;
 */
static __be32 nfsd3_proc_null(struct svc_rqst *rqstp)
{
	return rpc_success;
}

/*
 * Get the Access and/or Default ACL of a file.
 */
static __be32 nfsd3_proc_getacl(struct svc_rqst *rqstp)
{
	struct nfsd3_getaclargs *argp = rqstp->rq_argp;
	struct nfsd3_getaclres *resp = rqstp->rq_resp;
	struct posix_acl *acl;
	struct inode *inode;
	svc_fh *fh;

	fh = fh_copy(&resp->fh, &argp->fh);
	resp->status = fh_verify(rqstp, &resp->fh, 0, NFSD_MAY_NOP);
	if (resp->status != acl3_ok)
		goto out;

	inode = d_inode(fh->fh_dentry);

	if (argp->mask & ~NFS_ACL_MASK) {
		resp->status = acl3err_inval;
		goto out;
	}
	resp->mask = argp->mask;

	if (resp->mask & (NA_ACL | NA_ACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
		if (acl == NULL) {
			/* Solaris returns the inode's minimum ACL. */
			acl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);
		}
		if (IS_ERR(acl)) {
			resp->status = nfsd3_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_access = acl;
	}
	if (resp->mask & (NA_DFACL | NA_DFACLCNT)) {
		/* Check how Solaris handles requests for the Default ACL
		   of a non-directory! */
		acl = get_inode_acl(inode, ACL_TYPE_DEFAULT);
		if (IS_ERR(acl)) {
			resp->status = nfsd3_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_default = acl;
	}

	/* resp->acl_{access,default} are released in nfs3svc_release_getacl. */
out:
	resp->status = nfsd3_map_status(resp->status);
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
static __be32 nfsd3_proc_setacl(struct svc_rqst *rqstp)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;
	struct nfsd3_attrstat *resp = rqstp->rq_resp;
	struct inode *inode;
	svc_fh *fh;
	int error;

	fh = fh_copy(&resp->fh, &argp->fh);
	resp->status = fh_verify(rqstp, &resp->fh, 0, NFSD_MAY_SATTR);
	if (resp->status != acl3_ok)
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
	}

out_drop_lock:
	inode_unlock(inode);
	fh_drop_write(fh);
out_errno:
	resp->status = nfsd3_map_errno(error);
out:
	/* argp->acl_{access,default} are released in nfs3svc_release_setacl. */
	resp->status = nfsd3_map_status(resp->status);
	return rpc_success;
}

/*
 * XDR decode functions
 */

static bool
nfs3svc_decode_getaclargs(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_getaclargs *args = rqstp->rq_argp;

	if (!svcxdr_decode_nfs_fh3(xdr, &args->fh))
		return false;
	if (xdr_stream_decode_u32(xdr, &args->mask) < 0)
		return false;

	return true;
}

static bool
nfs3svc_decode_setaclargs(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;

	if (!svcxdr_decode_nfs_fh3(xdr, &argp->fh))
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

/*
 * XDR encode functions
 */

/* GETACL */
static bool
nfs3svc_encode_getaclres(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_getaclres *resp = rqstp->rq_resp;
	struct dentry *dentry = resp->fh.fh_dentry;
	struct inode *inode;

	if (!svcxdr_encode_nfsstat3(xdr, resp->status))
		return false;
	switch (resp->status) {
	case acl3_ok:
		inode = d_inode(dentry);
		if (!svcxdr_encode_post_op_attr(rqstp, xdr, &resp->fh))
			return false;
		if (xdr_stream_encode_u32(xdr, resp->mask) < 0)
			return false;

		if (!nfs_stream_encode_acl(xdr, inode, resp->acl_access,
					   resp->mask & NA_ACL, 0))
			return false;
		if (!nfs_stream_encode_acl(xdr, inode, resp->acl_default,
					   resp->mask & NA_DFACL,
					   NA_ACL_DEFAULT))
			return false;
		break;
	default:
		if (!svcxdr_encode_post_op_attr(rqstp, xdr, &resp->fh))
			return false;
	}

	return true;
}

/* SETACL */
static bool
nfs3svc_encode_setaclres(struct svc_rqst *rqstp, struct xdr_stream *xdr)
{
	struct nfsd3_attrstat *resp = rqstp->rq_resp;

	return svcxdr_encode_nfsstat3(xdr, resp->status) &&
		svcxdr_encode_post_op_attr(rqstp, xdr, &resp->fh);
}

/*
 * XDR release functions
 */
static void nfs3svc_release_getacl(struct svc_rqst *rqstp)
{
	struct nfsd3_getaclres *resp = rqstp->rq_resp;

	fh_put(&resp->fh);
	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
}

static void nfs3svc_release_setacl(struct svc_rqst *rqstp)
{
	struct nfsd3_setaclargs *argp = rqstp->rq_argp;
	struct nfsd3_attrstat *resp = rqstp->rq_resp;

	fh_put(&resp->fh);
	posix_acl_release(argp->acl_access);
	posix_acl_release(argp->acl_default);
}

#define ST 1		/* status*/
#define AT 21		/* attributes */
#define pAT (1+AT)	/* post attributes - conditional */
#define ACL (1+NFS_ACL_MAX_ENTRIES*3)  /* Access Control List */

static const struct svc_procedure nfsd_acl_procedures3[3] = {
	[ACLPROC3_NULL] = {
		.pc_func = nfsd3_proc_null,
		.pc_decode = xdrgen_svc_decode_void,
		.pc_encode = xdrgen_svc_encode_void,
		.pc_argsize = XDR_void,
		.pc_argzero = 0,
		.pc_ressize = 0,
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = XDR_void,
		.pc_name = "NULL",
	},
	[ACLPROC3_GETACL] = {
		.pc_func = nfsd3_proc_getacl,
		.pc_decode = nfs3svc_decode_getaclargs,
		.pc_encode = nfs3svc_encode_getaclres,
		.pc_release = nfs3svc_release_getacl,
		.pc_argsize = sizeof(struct nfsd3_getaclargs),
		.pc_argzero = sizeof(struct nfsd3_getaclargs),
		.pc_ressize = sizeof(struct nfsd3_getaclres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+1+2*(1+ACL),
		.pc_name = "GETACL",
	},
	[ACLPROC3_SETACL] = {
		.pc_func = nfsd3_proc_setacl,
		.pc_decode = nfs3svc_decode_setaclargs,
		.pc_encode = nfs3svc_encode_setaclres,
		.pc_release = nfs3svc_release_setacl,
		.pc_argsize = sizeof(struct nfsd3_setaclargs),
		.pc_argzero = sizeof(struct nfsd3_setaclargs),
		.pc_ressize = sizeof(struct nfsd3_attrstat),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+pAT,
		.pc_name = "SETACL",
	},
};

/*
 * Storage requirements for XDR arguments and results.
 */
union nfsd_acl3_xdrstore {
	struct nfsd3_getaclargs		getaclargs;
	struct nfsd3_setaclargs		setaclargs;
	struct nfsd3_getaclres		getaclres;
	struct nfsd3_attrstat		attrstat;
};

const struct svc_version nfsd_acl_version3 = {
	.vs_vers	= 3,
	.vs_nproc	= ARRAY_SIZE(nfsd_acl_procedures3),
	.vs_proc	= nfsd_acl_procedures3,
	.vs_dispatch	= nfsd_dispatch,
	.vs_xdrsize	= sizeof(union nfsd_acl3_xdrstore),
};

