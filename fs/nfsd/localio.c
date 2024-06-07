/*
 * NFS server support for local clients to bypass network stack
 *
 * Copyright (C) 2014 Weston Andros Adamson <dros@primarydata.com>
 */

#include <linux/exportfs.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs.h>
#include <linux/string.h>

#include "nfsd.h"
#include "vfs.h"
#include "netns.h"
#include "filecache.h"
#include "cache.h"
#include "xdr3.h"
#include "xdr4.h"

#define NFSDDBG_FACILITY		NFSDDBG_FH

static void
nfsd_local_fakerqst_destroy(struct svc_rqst *rqstp)
{
	if (rqstp->rq_client)
		auth_domain_put(rqstp->rq_client);
	if (rqstp->rq_cred.cr_group_info)
		put_group_info(rqstp->rq_cred.cr_group_info);
	kfree(rqstp->rq_cred.cr_principal);
	kfree(rqstp->rq_xprt);
	kfree(rqstp);
}

static struct svc_rqst *
nfsd_local_fakerqst_create(struct rpc_clnt *rpc_clnt, const struct cred *cred)
{
	struct svc_rqst *rqstp;
	struct net *net = rpc_net_ns(rpc_clnt);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	int status;

	if (!nn->nfsd_serv) {
		dprintk("%s: localio denied. Server not running\n", __func__);
		return ERR_PTR(-ENXIO);
	}

	rqstp = kzalloc(sizeof(*rqstp), GFP_KERNEL);
	if (!rqstp)
		return ERR_PTR(-ENOMEM);

	rqstp->rq_xprt = kzalloc(sizeof(*rqstp->rq_xprt), GFP_KERNEL);
	if (!rqstp->rq_xprt) {
		status = -ENOMEM;
		goto out_err;
	}

	rqstp->rq_xprt->xpt_net = net;
	__set_bit(RQ_SECURE, &rqstp->rq_flags);
	rqstp->rq_proc = 1;
	rqstp->rq_vers = 3;
	rqstp->rq_prot = IPPROTO_TCP;
	rqstp->rq_server = nn->nfsd_serv;

	/* Note: we're connecting to ourself, so source addr == peer addr */
	rqstp->rq_addrlen = rpc_peeraddr(rpc_clnt,
			(struct sockaddr *)&rqstp->rq_addr,
			sizeof(rqstp->rq_addr));

	if (!rpcauth_map_to_svc_cred(rpc_clnt->cl_auth, cred,
				     &rqstp->rq_cred)) {
		dprintk("%s :map cred failed\n", __func__);
		status = -EINVAL;
		goto out_err;
	}

	/*
	 * set up enough for svcauth_unix_set_client to be able to wait
	 * for the cache downcall. Note that we do _not_ want to allow the
	 * request to be deferred for later revisit since this rqst and xprt
	 * are not set up to run inside of the normal svc_rqst engine.
	 */
	INIT_LIST_HEAD(&rqstp->rq_xprt->xpt_deferred);
	kref_init(&rqstp->rq_xprt->xpt_ref);
	spin_lock_init(&rqstp->rq_xprt->xpt_lock);
	rqstp->rq_chandle.thread_wait = 5 * HZ;

	status = svcauth_unix_set_client(rqstp);
	switch (status) {
	case SVC_OK:
		break;
	case SVC_DENIED:
		status = -ENXIO;
		dprintk("%s: client %pISpc denied localio access\n",
				__func__, (struct sockaddr *)&rqstp->rq_addr);
		goto out_err;
	default:
		status = -ETIMEDOUT;
		dprintk("%s: client %pISpc temporarily denied localio access\n",
				__func__, (struct sockaddr *)&rqstp->rq_addr);
		goto out_err;
	}

	return rqstp;

out_err:
	nfsd_local_fakerqst_destroy(rqstp);
	return ERR_PTR(status);
}

/*
 * nfsd_open_local_fh - lookup a local filehandle @nfs_fh and map to @file
 *
 * This function maps a local fh to a path on a local filesystem.
 * This is useful when the nfs client has the local server mounted - it can
 * avoid all the NFS overhead with reads, writes and commits.
 *
 * on successful return, caller is responsible for calling path_put. Also
 * note that this is called from nfs.ko via find_symbol() to avoid an explicit
 * dependency on knfsd. So, there is no forward declaration in a header file
 * for it.
 */
int nfsd_open_local_fh(struct rpc_clnt *rpc_clnt,
			 const struct cred *cred,
			 const struct nfs_fh *nfs_fh,
			 const fmode_t fmode,
			 struct file **pfilp)
{
	const struct cred *save_cred;
	struct svc_rqst *rqstp;
	struct svc_fh fh;
	struct nfsd_file *nf;
	int status = 0;
	int mayflags = NFSD_MAY_LOCALIO;
	__be32 beres;

	/* Save creds before calling into nfsd */
	save_cred = get_current_cred();

	rqstp = nfsd_local_fakerqst_create(rpc_clnt, cred);
	if (IS_ERR(rqstp)) {
		status = PTR_ERR(rqstp);
		goto out_revertcred;
	}

	/* nfs_fh -> svc_fh */
	if (nfs_fh->size > NFS4_FHSIZE) {
		status = -EINVAL;
		goto out;
	}
	fh_init(&fh, NFS4_FHSIZE);
	fh.fh_handle.fh_size = nfs_fh->size;
	memcpy(fh.fh_handle.fh_raw, nfs_fh->data, nfs_fh->size);

	if (fmode & FMODE_READ)
		mayflags |= NFSD_MAY_READ;
	if (fmode & FMODE_WRITE)
		mayflags |= NFSD_MAY_WRITE;

	beres = nfsd_file_acquire(rqstp, &fh, mayflags, &nf);
	if (beres) {
		status = nfs_stat_to_errno(be32_to_cpu(beres));
		dprintk("%s: fh_verify failed %d\n", __func__, status);
		goto out_fh_put;
	}

	*pfilp = get_file(nf->nf_file);

	nfsd_file_put(nf);
out_fh_put:
	fh_put(&fh);

out:
	nfsd_local_fakerqst_destroy(rqstp);
out_revertcred:
	revert_creds(save_cred);
	return status;
}
EXPORT_SYMBOL_GPL(nfsd_open_local_fh);

/* Compile time type checking, not used by anything */
static nfs_to_nfsd_open_t __maybe_unused nfsd_open_local_fh_typecheck = nfsd_open_local_fh;

/*
 * GETUUID XDR encode functions
 */

static __be32 nfsd_proc_null(struct svc_rqst *rqstp)
{
	return rpc_success;
}

static __be32 nfsd_proc_getuuid(struct svc_rqst *rqstp)
{
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);
	struct nfsd_getuuidres *resp = rqstp->rq_resp;

	uuid_copy(&resp->uuid, &nn->nfsd_uuid.uuid);
	resp->status = nfs_ok;

	return rpc_success;
}

#if defined(CONFIG_NFSD_V3_LOCALIO)

static void encode_uuid(struct xdr_stream *xdr,
			const char *name, u32 length)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, 4 + length);
	xdr_encode_opaque(p, name, length);
}

static bool nfs3svc_encode_getuuidres(struct svc_rqst *rqstp,
				      struct xdr_stream *xdr)
{
	struct nfsd_getuuidres *resp = rqstp->rq_resp;

	if (!svcxdr_encode_nfsstat3(xdr, resp->status))
		return false;
	if (resp->status == nfs_ok) {
		u8 uuid[UUID_SIZE];

		export_uuid(uuid, &resp->uuid);
		encode_uuid(xdr, uuid, UUID_SIZE);
		dprintk("%s: nfs_ok uuid=%pU uuid_len=%lu\n",
			__func__, uuid, sizeof(uuid));
	}

	return true;
}

#define ST 1		/* status */
#define NFS3_filename_sz	(1+(NFS3_MAXNAMLEN>>2))

static const struct svc_procedure nfsd_localio_procedures3[2] = {
	[LOCALIOPROC_NULL] = {
		.pc_func = nfsd_proc_null,
		.pc_decode = nfssvc_decode_voidarg,
		.pc_encode = nfssvc_encode_voidres,
		.pc_argsize = sizeof(struct nfsd_voidargs),
		.pc_ressize = sizeof(struct nfsd_voidres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST,
		.pc_name = "NULL",
	},
	[LOCALIOPROC_GETUUID] = {
		.pc_func = nfsd_proc_getuuid,
		.pc_decode = nfssvc_decode_voidarg,
		.pc_encode = nfs3svc_encode_getuuidres,
		.pc_argsize = sizeof(struct nfsd_voidargs),
		.pc_ressize = sizeof(struct nfsd_getuuidres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+NFS3_filename_sz,
		.pc_name = "GETUUID",
	},
};

static DEFINE_PER_CPU_ALIGNED(unsigned long,
			      nfsd_localio_count3[ARRAY_SIZE(nfsd_localio_procedures3)]);
const struct svc_version nfsd_localio_version3 = {
	.vs_vers	= 3,
	.vs_nproc	= 2,
	.vs_proc	= nfsd_localio_procedures3,
	.vs_dispatch	= nfsd_dispatch,
	.vs_count	= nfsd_localio_count3,
	.vs_xdrsize	= NFS3_SVC_XDRSIZE,
};
#endif /* CONFIG_NFSD_V3_LOCALIO */

#if defined(CONFIG_NFSD_V4_LOCALIO)
static bool nfs4svc_encode_getuuidres(struct svc_rqst *rqstp,
				      struct xdr_stream *xdr)
{
	struct nfsd_getuuidres *resp = rqstp->rq_resp;
	__be32 *p;

	p = xdr_reserve_space(xdr, 8);
	if (!p)
		return 0;
	*p++ = cpu_to_be32(LOCALIOPROC_GETUUID);
	*p++ = resp->status;

	if (resp->status == nfs_ok) {
		u8 uuid[UUID_SIZE];

		export_uuid(uuid, &resp->uuid);
		p = xdr_reserve_space(xdr, 4 + UUID_SIZE);
		if (!p)
			return 0;
		xdr_encode_opaque(p, uuid, UUID_SIZE);
		dprintk("%s: nfs_ok uuid=%pU uuid_len=%lu\n",
			__func__, uuid, sizeof(uuid));
	}

	return 1;
}

#define ST 1		/* status */
#define NFS4_filename_sz	(1+(NFS4_MAXNAMLEN>>2))

static const struct svc_procedure nfsd_localio_procedures4[2] = {
	[LOCALIOPROC_NULL] = {
		.pc_func = nfsd_proc_null,
		.pc_decode = nfssvc_decode_voidarg,
		.pc_encode = nfssvc_encode_voidres,
		.pc_argsize = sizeof(struct nfsd_voidargs),
		.pc_ressize = sizeof(struct nfsd_voidres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST,
		.pc_name = "NULL",
	},
	[LOCALIOPROC_GETUUID] = {
		.pc_func = nfsd_proc_getuuid,
		.pc_decode = nfssvc_decode_voidarg,
		.pc_encode = nfs4svc_encode_getuuidres,
		.pc_argsize = sizeof(struct nfsd_voidargs),
		.pc_ressize = sizeof(struct nfsd_getuuidres),
		.pc_cachetype = RC_NOCACHE,
		.pc_xdrressize = ST+NFS4_filename_sz,
		.pc_name = "GETUUID",
	},
};

static DEFINE_PER_CPU_ALIGNED(unsigned long,
			      nfsd_localio_count4[ARRAY_SIZE(nfsd_localio_procedures4)]);
const struct svc_version nfsd_localio_version4 = {
	.vs_vers	        = 4,
	.vs_nproc	        = 2,
	.vs_proc	        = nfsd_localio_procedures4,
	.vs_dispatch	        = nfsd_dispatch,
	.vs_count	        = nfsd_localio_count4,
	.vs_xdrsize	        = NFS4_SVC_XDRSIZE,
	.vs_rpcb_optnl		= true,
	.vs_need_cong_ctrl	= true,

};
#endif /* CONFIG_NFSD_V4_LOCALIO */
