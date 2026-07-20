// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMB2 compression support for ksmbd.
 *
 * Receive and send SMB 3.1.1 compression transforms using the common helpers.
 *
 * Copyright (C) 2026 Namjae Jeon <linkinjeon@kernel.org>
 */
#include <linux/slab.h>

#include "compress.h"
#include "smb_common.h"

/**
 * ksmbd_decompress_request() - replace a compressed request with its SMB2 PDU
 * @conn: connection which owns the current RFC1002 request buffer
 *
 * Derive the uncompressed size from the transform variant, enforce ksmbd's
 * normal message limits, and ask the common decoder to validate every payload.
 * On success, replace conn->request_buf with a regular RFC1002-framed SMB2
 * message so the rest of the request path needs no compression awareness.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int ksmbd_decompress_request(struct ksmbd_conn *conn)
{
	unsigned int pdu_size = get_rfc1002_len(conn->request_buf);
	char *buf, *out;
	u32 out_size;
	int rc;

	if (pdu_size < sizeof(struct smb2_compression_hdr))
		return -EINVAL;

	if (conn->dialect != SMB311_PROT_ID ||
	    !smb_compress_alg_valid(conn->compress_algorithm, false))
		return -EINVAL;

	buf = smb_get_msg(conn->request_buf);
	out_size = smb_decompress_alloc_size(buf, pdu_size, 0,
					     SMB3_MAX_MSGSIZE + conn->vals->max_write_size,
					     conn->compress_chained);
	if (out_size == 0 || out_size > MAX_STREAM_PROT_LEN)
		return -EINVAL;

	out = kvmalloc(out_size + 4 + 1, KSMBD_DEFAULT_GFP);
	if (!out)
		return -ENOMEM;

	*(__be32 *)out = cpu_to_be32(out_size);
	rc = smb_compression_decompress(conn->compress_algorithm,
					conn->compress_chained,
					buf, pdu_size, out + 4, out_size);
	if (rc) {
		kvfree(out);
		return rc;
	}

	kvfree(conn->request_buf);
	conn->request_buf = out;
	return 0;
}

/**
 * ksmbd_compress_response() - compress an eligible ksmbd response
 * @work: request work item containing the response iov
 *
 * Compression transforms describe one contiguous SMB2 message, while ksmbd
 * builds responses from multiple iov entries. Flatten the response first,
 * produce the negotiated transform, and replace the response iov only when the
 * result is smaller than the original message.
 *
 * Encrypted and compound responses are intentionally left unchanged. The
 * caller may still continue sending the original response when this function
 * returns zero.
 *
 * Return: 1 if the response was replaced, 0 if compression was skipped, or a
 * negative errno on failure.
 */
int ksmbd_compress_response(struct ksmbd_work *work)
{
	struct smb2_hdr *req_hdr;
	u8 *src = NULL, *out = NULL, *p;
	bool chained, pattern;
	u32 src_len, dst_len;
	__le16 alg = work->conn->compress_algorithm;
	int i, rc;

	if (!work->compress_response || work->encrypted ||
	    !smb_compress_alg_valid(alg, false))
		return 0;

	req_hdr = smb_get_msg(work->request_buf);
	if (req_hdr->NextCommand || work->next_smb2_rcv_hdr_off ||
	    work->next_smb2_rsp_hdr_off)
		return 0;

	src_len = get_rfc1002_len(work->iov[0].iov_base);
	if (src_len < SMB_COMPRESS_MIN_LEN)
		return 0;

	src = kvmalloc(src_len, KSMBD_DEFAULT_GFP);
	if (!src)
		return -ENOMEM;

	p = src;
	/* iov[0] contains only the RFC1002 length; the SMB2 PDU starts at iov[1]. */
	for (i = 1; i < work->iov_cnt; i++) {
		if (work->iov[i].iov_len > src + src_len - p) {
			rc = -EINVAL;
			goto out;
		}
		memcpy(p, work->iov[i].iov_base, work->iov[i].iov_len);
		p += work->iov[i].iov_len;
	}
	if (p != src + src_len) {
		rc = -EINVAL;
		goto out;
	}

	chained = work->conn->compress_chained;
	pattern = work->conn->compress_pattern;
	if (unlikely(!chained && pattern)) {
		rc = -EINVAL;
		goto out;
	}

	dst_len = smb_compress_alloc_size(src_len, chained, pattern, alg);
	out = kvzalloc(sizeof(__be32) + dst_len,
		       KSMBD_DEFAULT_GFP);
	if (!out) {
		rc = -ENOMEM;
		goto out;
	}

	rc = smb_compression_compress(alg, chained, pattern,
				      src, src_len,
				      out + sizeof(__be32),
				      &dst_len);
	/* if dst_len >= src_len, rc is 0 */
	if (rc || dst_len >= src_len)
		goto out;

	*(__be32 *)out = cpu_to_be32(dst_len);

	/*
	 * Keep the transform in work->compress_buf until send completion.
	 * Existing response iovs can then be replaced without changing their
	 * individual ownership rules.
	 */
	work->compress_buf = out;
	work->iov[0].iov_base = out;
	work->iov[0].iov_len = sizeof(__be32);
	work->iov[1].iov_base = out + sizeof(__be32);
	work->iov[1].iov_len = dst_len;
	work->iov_cnt = 2;
	work->iov_idx = 1;
	out = NULL;
	rc = 1;
out:
	kvfree(out);
	kvfree(src);
	return rc;
}
