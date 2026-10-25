// SPDX-License-Identifier: GPL-2.0
/*
 * XDR support for nfsd/protocol version 3.
 *
 * Copyright (C) 1995, 1996, 1997 Olaf Kirch <okir@monad.swb.de>
 *
 * 2003-08-09 Jamie Lokier: Use htonl() for nanoseconds, not htons()!
 */

#include <linux/namei.h>
#include <linux/sunrpc/svc_xprt.h>

#include "nfsd.h"
#include "xdr3.h"
#include "auth.h"
#include "netns.h"
#include "vfs.h"
#include "nfserr.h"
#include "trace.h"

#include "nfs3xdr_gen.h"

/*
 * XDR encode functions
 */

/*
 * READDIR reply entry list (RFC 1813).  The entry list is the
 * value-follows form of "entry3 *entries": each entry prefixed by
 * TRUE, the sequence closed by FALSE.  xdrgen's aggregate codec owns
 * that framing; the hooks below stream one entry at a time straight
 * from the directory into the live reply, mirroring
 * nfsd4_encode_dirlist4.  The directory was opened in
 * nfsd3_proc_readdir(), so reading and encoding happen together here
 * during reply encoding.
 */

/**
 * nfs3_dirlist3_encode_begin - size the streaming entry budget
 * @c: aggregate cursor for the entry list
 *
 * Derive the entry byte budget from the space left in the live reply
 * and the client's count hint, reserving room for the list terminator
 * and the eof flag that follow the entries.
 *
 * Return: true.
 */
bool nfs3_dirlist3_encode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = c->xdr;
	int bytes_left;

	/* Reserve the terminator FALSE and the eof bool (two words). */
	bytes_left = xdr->buf->buflen - xdr->buf->len - XDR_UNIT * 2;
	if (bytes_left < 0)
		bytes_left = 0;
	resp->space_left = min_t(u32, bytes_left,
				 clamp(resp->count, (u32)(XDR_UNIT * 2),
				       (u32)svc_max_payload(rqstp)) -
				 XDR_UNIT * 2);
	resp->cookie_offset = 0;
	return true;
}

/**
 * nfs3_dirlist3_encode - stream the next READDIR entry
 * @c: aggregate cursor for the entry list
 * @out: OUT: entry the framing encodes when one is produced
 *
 * Back-patch the previous entry's placeholder cookie with this entry's
 * resume offset, then, if the reply budget allows, project the next
 * directory entry into @out for the generated per-entry encoder.
 *
 * Return: true when @out holds an entry to encode; false to end the
 * list -- because the directory is exhausted, a host error struck, or
 * the reply budget filled.  On budget exhaustion the previous entry's
 * cookie already points at the rejected entry, the client's resume
 * point.
 */
bool nfs3_dirlist3_encode(struct xdrgen_aggregate_cursor *c,
			  struct entry3 *out)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = c->xdr;
	struct buffered_dirent *de;
	int namlen;
	u32 need;

	/*
	 * The previous entry was just encoded; commit the stream and note
	 * its cookie slot -- the entry's last two XDR words -- so it can be
	 * back-patched once this entry's resume offset is known.
	 */
	if (c->index) {
		xdr_commit_encode(xdr);
		resp->cookie_offset = xdr->buf->len - XDR_UNIT * 2;
	}

	de = nfsd_readdir_next(&resp->iter);
	if (!de)
		return false;

	/* The previous entry's cookie is this entry's resume offset. */
	nfs3svc_encode_cookie3(xdr, resp->cookie_offset, de->offset);

	namlen = min_t(int, de->namlen, NFS3_MAXNAMLEN);

	/* value-follows + fileid + name (length + data) + cookie */
	need = XDR_UNIT * (6 + XDR_QUADLEN(namlen));
	if (need > resp->space_left)
		return false;
	resp->space_left -= need;

	out->fileid = de->ino;
	out->name.len = namlen;
	out->name.data = (unsigned char *)de->name;
	out->cookie = 0;	/* back-patched later */

	trace_nfsd_dirent(resp->iter.fhp, de->ino, de->name, namlen);
	return true;
}

/*
 * The xdr_stream primitives do not advance rq_next_page, and svcrdma
 * retains only the pages below it for Send completion. The eof word that
 * follows the entry list starts a new page when this one is full.
 */
static void nfs3svc_update_next_page(struct svc_rqst *rqstp,
				     struct xdr_stream *xdr)
{
	rqstp->rq_next_page = xdr->page_ptr + 1;
	if (xdr->p == xdr->end)
		rqstp->rq_next_page++;
}

/**
 * nfs3_dirlist3_encode_end - finish the streamed entry list
 * @c: aggregate cursor for the entry list
 * @ok: false if the framing hit a wire error while encoding the list
 *
 * Back-patch the final entry's cookie with the directory's resume
 * offset and report eof.  A list cut short by the reply budget or a
 * host error reports eof false, so the client reads the rest with a
 * follow-up request.
 *
 * Return: true.
 */
bool nfs3_dirlist3_encode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = c->xdr;

	if (ok)
		nfs3svc_encode_cookie3(xdr, resp->cookie_offset,
				       resp->iter.offset);
	resp->xdrgen.u.resok.reply.eof = resp->iter.eof;
	nfs3svc_update_next_page(rqstp, xdr);
	return true;
}

static __be32
compose_entry_fh(struct nfsd3_readdirplusres *cd, struct svc_fh *fhp,
		 const char *name, int namlen, u64 ino)
{
	struct svc_export	*exp;
	struct dentry		*dparent, *dchild;
	__be32 rv = nfserr_noent;

	dparent = cd->iter.fhp->fh_dentry;
	exp  = cd->iter.fhp->fh_export;

	if (name_is_dot_dotdot(name, namlen)) {
		if (namlen == 2) {
			dchild = dget_parent(dparent);
			/*
			 * Don't return filehandle for ".." if we're at
			 * the filesystem or export root:
			 */
			if (dchild == dparent)
				goto out;
			if (dparent == exp->ex_path.dentry)
				goto out;
		} else
			dchild = dget(dparent);
	} else
		dchild = lookup_one_positive_unlocked(&nop_mnt_idmap,
						      &QSTR_LEN(name, namlen),
						      dparent);
	if (IS_ERR(dchild))
		return rv;
	if (d_mountpoint(dchild))
		goto out;
	if (dchild->d_inode->i_ino != ino)
		goto out;
	rv = fh_compose(fhp, exp, dchild, cd->iter.fhp);
out:
	dput(dchild);
	return rv;
}

/**
 * nfs3svc_encode_cookie3 - Encode a directory cookie
 * @xdr: stream into which to encode the cookie
 * @pos: byte position in the stream
 * @cookie: cookie to be encoded
 *
 * The buffer space for the offset cookie has already been reserved
 * by nfs3_dirlist3_encode() or nfs3_dirlistplus3_encode().
 */
void nfs3svc_encode_cookie3(struct xdr_stream *xdr, unsigned int pos,
			    u64 cookie)
{
	__be64 wire_cookie = cpu_to_be64(cookie);

	if (!pos)
		return;
	write_bytes_to_xdr_buf(xdr->buf, pos, &wire_cookie, XDR_UNIT * 2);
}

/*
 * READDIRPLUS reply entry list (RFC 1813).  Like READDIR's entry list,
 * the entries are the value-follows form of "entryplus3 *entries" and
 * xdrgen's aggregate codec owns the framing.  Each entry additionally
 * carries the object's post-op attributes and file handle, composed
 * and stat'ed here as the entry is streamed.  The directory was opened
 * in nfsd3_proc_readdirplus(), so reading and encoding happen together
 * during reply encoding.
 */

/**
 * nfs3_dirlistplus3_encode_begin - size the streaming entry budget
 * @c: aggregate cursor for the entry list
 *
 * Derive the entry byte budget from the space left in the live reply
 * and the client's maxcount hint, reserving room for the list
 * terminator and the eof flag that follow the entries.
 *
 * Return: true.
 */
bool nfs3_dirlistplus3_encode_begin(struct xdrgen_aggregate_cursor *c)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirplusres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = c->xdr;
	int bytes_left;

	/* Reserve the terminator FALSE and the eof bool (two words). */
	bytes_left = xdr->buf->buflen - xdr->buf->len - XDR_UNIT * 2;
	if (bytes_left < 0)
		bytes_left = 0;
	resp->space_left = min_t(u32, bytes_left,
				 clamp(resp->count, (u32)(XDR_UNIT * 2),
				       (u32)svc_max_payload(rqstp)) -
				 XDR_UNIT * 2);
	resp->cookie_offset = 0;
	return true;
}

/**
 * nfs3_dirlistplus3_encode - stream the next READDIRPLUS entry
 * @c: aggregate cursor for the entry list
 * @out: OUT: entry the framing encodes when one is produced
 *
 * Back-patch the previous entry's placeholder cookie with this entry's
 * resume offset, compose the entry's post-op attributes and file
 * handle, then, if the reply budget allows, project the next directory
 * entry into @out for the generated per-entry encoder.  @out's file
 * handle bytes live in a reply-owned scratch buffer, valid until the
 * generated encoder writes them and the following pull overwrites them.
 *
 * Return: true when @out holds an entry to encode; false to end the
 * list -- because the directory is exhausted, a host error struck, or
 * the reply budget filled.  On budget exhaustion the previous entry's
 * cookie already points at the rejected entry, the client's resume
 * point.
 */
bool nfs3_dirlistplus3_encode(struct xdrgen_aggregate_cursor *c,
			      struct entryplus3 *out)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirplusres *resp = rqstp->rq_resp;
	struct svc_fh *scratch = &resp->scratch;
	struct xdr_stream *xdr = c->xdr;
	struct buffered_dirent *de;
	int namlen;
	u32 need;

	/*
	 * Commit the previous entry so its cookie slot (recorded in
	 * resp->cookie_offset) is safe to back-patch below.  Unlike
	 * entry3, an entryplus3's cookie is not its final field, so the
	 * slot cannot be derived from buf->len after the fact; it is
	 * computed from the entry's start position instead (see the end
	 * of this function).
	 */
	if (c->index)
		xdr_commit_encode(xdr);

	de = nfsd_readdir_next(&resp->iter);
	if (!de)
		return false;

	/* The previous entry's cookie is this entry's resume offset. */
	nfs3svc_encode_cookie3(xdr, resp->cookie_offset, de->offset);

	namlen = min_t(int, de->namlen, NFS3_MAXNAMLEN);

	out->fileid = de->ino;
	out->name.len = namlen;
	out->name.data = (unsigned char *)de->name;
	out->cookie = 0;	/* back-patched later */

	/*
	 * value-follows + fileid + name (length + data) + cookie +
	 * attributes_follow + handle_follows. Rejecting on this
	 * minimum first spares the lookup and getattr below.
	 */
	need = XDR_UNIT * (8 + XDR_QUADLEN(namlen));
	if (need > resp->space_left)
		return false;

	/* Compose the entry's post-op attributes and file handle. */
	fh_init(scratch, NFS3_FHSIZE);
	if (compose_entry_fh(resp, scratch, de->name, namlen, de->ino) == nfs_ok) {
		nfsd3_fill_post_op_attr(rqstp, &out->name_attributes, scratch);
		nfsd3_fill_post_op_fh3(&out->name_handle, scratch, resp->fh_data);
	} else {
		out->name_attributes.attributes_follow = false;
		out->name_handle.handle_follows = false;
	}
	fh_put(scratch);

	/* optional fattr3 */
	if (out->name_attributes.attributes_follow)
		need += XDR_UNIT * 21;
	/* optional nfs_fh3 */
	if (out->name_handle.handle_follows)
		need += XDR_UNIT * (1 + XDR_QUADLEN(out->name_handle.u.handle.data.len));
	if (need > resp->space_left)
		return false;
	resp->space_left -= need;

	/*
	 * buf->len marks where the generated encoder begins this entry.
	 * Its cookie follows the value-follows flag, the fileid, and the
	 * name; record that slot for the next entry (or the end hook) to
	 * back-patch once the resume offset is known.
	 */
	resp->cookie_offset = xdr->buf->len +
			      XDR_UNIT * (4 + XDR_QUADLEN(namlen));

	trace_nfsd_dirent(resp->iter.fhp, de->ino, de->name, namlen);
	return true;
}

/**
 * nfs3_dirlistplus3_encode_end - finish the streamed entry list
 * @c: aggregate cursor for the entry list
 * @ok: false if the framing hit a wire error while encoding the list
 *
 * Back-patch the final entry's cookie with the directory's resume
 * offset and report eof.  A list cut short by the reply budget or a
 * host error reports eof false, so the client reads the rest with a
 * follow-up request.
 *
 * Return: true.
 */
bool nfs3_dirlistplus3_encode_end(struct xdrgen_aggregate_cursor *c, bool ok)
{
	struct svc_rqst *rqstp = c->ctx;
	struct nfsd3_readdirplusres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = c->xdr;

	if (ok)
		nfs3svc_encode_cookie3(xdr, resp->cookie_offset,
				       resp->iter.offset);
	resp->xdrgen.u.resok.reply.eof = resp->iter.eof;
	nfs3svc_update_next_page(rqstp, xdr);
	return true;
}

/*
 * XDR release functions
 */
void
nfs3svc_release_readdirres(struct svc_rqst *rqstp)
{
	struct nfsd3_readdirres *resp = rqstp->rq_resp;
	struct READDIR3args_wrapper *argp = rqstp->rq_argp;

	/* argp->fh is zeroed by pc_argzero when argument decoding fails */
	nfsd_readdir_close(&resp->iter);
	fh_put(&argp->fh);
}

void
nfs3svc_release_readdirplusres(struct svc_rqst *rqstp)
{
	struct nfsd3_readdirplusres *resp = rqstp->rq_resp;
	struct READDIRPLUS3args_wrapper *argp = rqstp->rq_argp;

	nfsd_readdir_close(&resp->iter);
	fh_put(&argp->fh);
}
