/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_ACOMP_WORKSPACE_H
#define BTRFS_ACOMP_WORKSPACE_H

#include <crypto/acompress.h>
#include <linux/scatterlist.h>

#include "compression.h"

/*
 * Maximum number of scatterlist entries needed for btrfs compression.
 * Based on BTRFS_MAX_COMPRESSED_PAGES (32 pages max).
 */
#define BTRFS_ACOMP_MAX_SGL_ENTRIES	BTRFS_MAX_COMPRESSED_PAGES

/* Maximum size needed for compression attribute buffer */
#define BTRFS_ACOMP_ATTR_BUF_SIZE	64

struct btrfs_acomp_workspace {
	struct scatterlist in_sgl[BTRFS_ACOMP_MAX_SGL_ENTRIES];
	struct scatterlist out_sgl[BTRFS_ACOMP_MAX_SGL_ENTRIES];
	struct folio *folios[BTRFS_ACOMP_MAX_SGL_ENTRIES];
	u8 attr_buffer[BTRFS_ACOMP_ATTR_BUF_SIZE];
	unsigned int attr_buf_len;
	const char *alg_name;
	struct crypto_acomp *tfm;
	struct acomp_req *req;
};

static inline struct scatterlist *btrfs_acomp_get_input_sgl(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->in_sgl : NULL;
}

static inline struct scatterlist *btrfs_acomp_get_output_sgl(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->out_sgl : NULL;
}

static inline struct folio **btrfs_acomp_get_folios(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->folios : NULL;
}

static inline u8 *btrfs_acomp_get_attr_buffer(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->attr_buffer : NULL;
}

static inline struct crypto_acomp *btrfs_acomp_get_tfm(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->tfm : NULL;
}

static inline struct acomp_req *btrfs_acomp_get_request(struct btrfs_acomp_workspace *acomp_ws)
{
	return acomp_ws ? acomp_ws->req : NULL;
}

#endif /* BTRFS_ACOMP_WORKSPACE_H */
