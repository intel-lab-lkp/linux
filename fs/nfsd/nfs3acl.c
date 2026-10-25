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
#include "auth.h"
#include "vfs.h"
#include "fattr.h"

#define NFS_ACL_MASK		(NA_ACL | NA_ACLCNT | NA_DFACL | NA_DFACLCNT)

/*
 * Wrapper structures combine xdrgen types with legacy structures.
 * The xdrgen field must be first so the structure can be cast
 * to its XDR type for the RPC dispatch layer.
 */
struct GETACL3args_wrapper {
	struct GETACL3args	xdrgen;
	struct svc_fh		fh;
};

static_assert(offsetof(struct GETACL3args_wrapper, xdrgen) == 0);

struct GETACL3res_wrapper {
	struct GETACL3res	xdrgen;
	int			mask;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
	kuid_t			uid;
	kgid_t			gid;
};

static_assert(offsetof(struct GETACL3res_wrapper, xdrgen) == 0);

struct SETACL3args_wrapper {
	struct SETACL3args	xdrgen;
	struct svc_fh		fh;
	struct posix_acl	*acl_access;
	struct posix_acl	*acl_default;
	/*
	 * Set when the wire ACL is well-formed XDR but semantically
	 * invalid. The decoder accepts the request so that
	 * nfsd3_proc_setacl can return ACL3ERR_INVAL rather than RPC
	 * GARBAGE_ARGS. See draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
	 */
	bool			invalid;
};

static_assert(offsetof(struct SETACL3args_wrapper, xdrgen) == 0);

struct SETACL3res_wrapper {
	struct SETACL3res	xdrgen;
};

static_assert(offsetof(struct SETACL3res_wrapper, xdrgen) == 0);

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
	case acl3_ok:
	case acl3err_perm:
	case acl3err_noent:
	case acl3err_io:
	case acl3err_acces:
	case acl3err_inval:
	case acl3err_nospc:
	case acl3err_rofs:
	case acl3err_dquot:
	case acl3err_stale:
	case acl3err_badhandle:
	case acl3err_notsupp:
	case acl3err_serverfault:
	case acl3err_jukebox:
		return status;
	case cpu_to_be32(NFS4ERR_NOFILEHANDLE):
		return acl3err_badhandle;
	case cpu_to_be32(NFS4ERR_WRONGSEC):
		return acl3err_acces;
	default:
		return acl3err_serverfault;
	}
}

static bool
nfs3acl_decode_fh(const struct nfs_fh3 *fh, struct svc_fh *fhp)
{
	if (fh->data.len == 0 || fh->data.len > NFS3_FHSIZE)
		return false;
	fh_init(fhp, NFS3_FHSIZE);
	fhp->fh_handle.fh_size = fh->data.len;
	memcpy(&fhp->fh_handle.fh_raw, fh->data.data, fh->data.len);
	return true;
}

/*
 * A minimal (three-entry) ACL travels as four wire entries: the
 * POSIX entries plus a synthesized NA_CLASS_OBJ mask. The wire
 * element count is thus max(a_count, 4) when the ACL has at least
 * three entries, and zero otherwise. An ACL with fewer than three
 * entries has no minimal set to expand, so it travels as empty.
 * See draft-ietf-nfsv4-nfs-acl Section 3.4.4.2.
 */
static u32 nfs3acl_acl_entries(const struct posix_acl *acl)
{
	return (acl && acl->a_count >= 3) ? max_t(int, acl->a_count, 4) : 0;
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

/**
 * nfsd3_proc_getacl - GETACL: Get the Access and/or Default ACL of a file
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   GETACL3res ACLPROC3_GETACL(GETACL3args) = 1;
 */
static __be32 nfsd3_proc_getacl(struct svc_rqst *rqstp)
{
	struct GETACL3args_wrapper *argp = rqstp->rq_argp;
	struct GETACL3res_wrapper *resp = rqstp->rq_resp;
	struct svc_fh *fhp = &argp->fh;
	struct posix_acl *acl;
	struct inode *inode;

	resp->xdrgen.u.resok.attr.attributes_follow = false;

	/* An unparsable handle leaves @fhp uninitialized, so skip fh_put. */
	if (!nfs3acl_decode_fh(&argp->xdrgen.fh, fhp)) {
		resp->xdrgen.status = acl3err_badhandle;
		return rpc_success;
	}

	resp->xdrgen.status = fh_verify(rqstp, fhp, 0, NFSD_MAY_NOP);
	if (resp->xdrgen.status != acl3_ok)
		goto out;
	inode = d_inode(fhp->fh_dentry);

	nfsd3_fill_post_op_attr(rqstp, &resp->xdrgen.u.resok.attr, fhp);

	if (argp->xdrgen.mask & ~NFS_ACL_MASK) {
		resp->xdrgen.status = acl3err_inval;
		goto out;
	}
	resp->mask = argp->xdrgen.mask;
	resp->uid = inode->i_uid;
	resp->gid = inode->i_gid;

	if (resp->mask & (NA_ACL | NA_ACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
		if (acl == NULL) {
			/* Solaris returns the inode's minimum ACL. */
			acl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);
		}
		if (IS_ERR(acl)) {
			resp->xdrgen.status = nfsd3_map_errno(PTR_ERR(acl));
			goto fail;
		}
		resp->acl_access = acl;
	}
	if (resp->mask & (NA_DFACL | NA_DFACLCNT)) {
		acl = get_inode_acl(inode, ACL_TYPE_DEFAULT);
		if (IS_ERR(acl)) {
			resp->xdrgen.status = nfsd3_map_errno(PTR_ERR(acl));
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
	resp->xdrgen.u.resok.acl.aclcnt = nfs3acl_acl_entries(resp->acl_access);
	resp->xdrgen.u.resok.acl.dfaclcnt = nfs3acl_acl_entries(resp->acl_default);

out:
	/* resp->acl_{access,default} are released in nfs3svc_release_getacl. */
	fh_put(fhp);
	resp->xdrgen.status = nfsd3_map_status(resp->xdrgen.status);
	return rpc_success;

fail:
	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
	resp->acl_access = NULL;
	resp->acl_default = NULL;
	goto out;
}

/**
 * nfsd3_proc_setacl - SETACL: Set the Access and/or Default ACL of a file
 * @rqstp: RPC transaction context
 *
 * Return:
 *   %rpc_success:		RPC executed successfully
 *
 * RPC synopsis:
 *   SETACL3res ACLPROC3_SETACL(SETACL3args) = 2;
 */
static __be32 nfsd3_proc_setacl(struct svc_rqst *rqstp)
{
	struct SETACL3args_wrapper *argp = rqstp->rq_argp;
	struct SETACL3res_wrapper *resp = rqstp->rq_resp;
	struct svc_fh *fhp = &argp->fh;
	struct inode *inode;
	int error;

	resp->xdrgen.u.resok.attr.attributes_follow = false;

	if (!nfs3acl_decode_fh(&argp->xdrgen.fh, fhp)) {
		resp->xdrgen.status = acl3err_badhandle;
		return rpc_success;
	}

	resp->xdrgen.status = fh_verify(rqstp, fhp, 0, NFSD_MAY_SATTR);
	if (resp->xdrgen.status != acl3_ok)
		goto out;
	inode = d_inode(fhp->fh_dentry);

	if (argp->invalid) {
		resp->xdrgen.status = acl3err_inval;
		goto out;
	}

	/* The VFS reports a default ACL on a non-directory as EACCES. */
	if (argp->acl_default && !S_ISDIR(inode->i_mode)) {
		resp->xdrgen.status = acl3err_inval;
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

out:
	/*
	 * resok and resfail both open with a post_op_attr, so filling
	 * the resok arm serves either status.
	 */
	nfsd3_fill_post_op_attr(rqstp, &resp->xdrgen.u.resok.attr, fhp);
	/* argp->acl_{access,default} are released in nfs3svc_release_setacl. */
	fh_put(fhp);
	resp->xdrgen.status = nfsd3_map_status(resp->xdrgen.status);
	return rpc_success;

out_drop_lock:
	inode_unlock(inode);
	fh_drop_write(fhp);
out_errno:
	resp->xdrgen.status = nfsd3_map_errno(error);
	goto out;
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
 * NFS_ACL3_SECATTR_ACLENT or NFS_ACL3_SECATTR_DFACLENT.
 */

/* GETACL reply: stream the wire entries out of the source posix_acl. */

bool nfs_acl3_secattr_encode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct GETACL3res_wrapper *resp = rqstp->rq_resp;
	bool dfacl = c->member_id == NFS_ACL3_SECATTR_DFACLENT;
	struct posix_acl *acl = dfacl ? resp->acl_default : resp->acl_access;
	int gate = dfacl ? (resp->mask & NA_DFACL) : (resp->mask & NA_ACL);

	c->count = gate ? nfs3acl_acl_entries(acl) : 0;
	return true;
}

bool nfs_acl3_secattr_encode(struct xdrgen_aggregate_cursor *c,
			     struct aclent *out)
{
	struct svc_rqst *rqstp = c->ctx;
	struct GETACL3res_wrapper *resp = rqstp->rq_resp;
	bool dfacl = c->member_id == NFS_ACL3_SECATTR_DFACLENT;
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
		out->id = from_kuid(&init_user_ns, resp->uid);
		break;
	case ACL_GROUP_OBJ:
		out->id = from_kgid(&init_user_ns, resp->gid);
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
bool nfs_acl3_secattr_encode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct xdr_stream *xdr = c->xdr;

	rqstp->rq_next_page = xdr->page_ptr + 1;
	if (xdr->p == xdr->end)
		rqstp->rq_next_page++;
	return true;
}

/* SETACL args: consume the decoded wire entries into the target posix_acl. */

bool nfs_acl3_secattr_decode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL3args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL3_SECATTR_DFACLENT;
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

bool nfs_acl3_secattr_decode(struct xdrgen_aggregate_cursor *c,
			     const struct aclent *in)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL3args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL3_SECATTR_DFACLENT;
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

	/* A type bit the protocol does not define makes the ACL invalid. */
	if (in->type & ~(NA_USER_OBJ | NA_USER | NA_GROUP_OBJ | NA_GROUP |
			 NA_CLASS_OBJ | NA_OTHER_OBJ | NA_ACL_DEFAULT)) {
		argp->invalid = true;
		return true;
	}

	entry = &acl->a_entries[c->index];
	entry->e_tag = in->type & ~NA_ACL_DEFAULT;
	entry->e_perm = in->perm;
	switch (entry->e_tag) {
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

	return true;
}

bool nfs_acl3_secattr_decode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct SETACL3args_wrapper *argp = rqstp->rq_argp;
	bool dfacl = c->member_id == NFS_ACL3_SECATTR_DFACLENT;
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
 * XDR release functions
 */
static void nfs3svc_release_getacl(struct svc_rqst *rqstp)
{
	struct GETACL3res_wrapper *resp = rqstp->rq_resp;

	posix_acl_release(resp->acl_access);
	posix_acl_release(resp->acl_default);
}

static void nfs3svc_release_setacl(struct svc_rqst *rqstp)
{
	struct SETACL3args_wrapper *argp = rqstp->rq_argp;

	posix_acl_release(argp->acl_access);
	posix_acl_release(argp->acl_default);
}

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
		.pc_decode = nfs_acl_svc_decode_GETACL3args,
		.pc_encode = nfs_acl_svc_encode_GETACL3res,
		.pc_release = nfs3svc_release_getacl,
		.pc_argsize = sizeof(struct GETACL3args_wrapper),
		.pc_argzero = 0,
		.pc_ressize = sizeof(struct GETACL3res_wrapper),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = NFS_ACL3_GETACL3res_sz,
		.pc_name = "GETACL",
	},
	[ACLPROC3_SETACL] = {
		.pc_func = nfsd3_proc_setacl,
		.pc_decode = nfs_acl_svc_decode_SETACL3args,
		.pc_encode = nfs_acl_svc_encode_SETACL3res,
		.pc_release = nfs3svc_release_setacl,
		.pc_argsize = sizeof(struct SETACL3args_wrapper),
		.pc_argzero = sizeof(struct SETACL3args_wrapper),
		.pc_ressize = sizeof(struct SETACL3res_wrapper),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = NFS_ACL3_SETACL3res_sz,
		.pc_name = "SETACL",
	},
};

/*
 * Storage requirements for XDR arguments and results.
 */
union nfsd_acl3_xdrstore {
	struct GETACL3args_wrapper	getaclargs;
	struct SETACL3args_wrapper	setaclargs;
	struct GETACL3res_wrapper	getaclres;
	struct SETACL3res_wrapper	setaclres;
};

const struct svc_version nfsd_acl_version3 = {
	.vs_vers	= 3,
	.vs_nproc	= ARRAY_SIZE(nfsd_acl_procedures3),
	.vs_proc	= nfsd_acl_procedures3,
	.vs_dispatch	= nfsd_dispatch,
	.vs_xdrsize	= sizeof(union nfsd_acl3_xdrstore),
};
