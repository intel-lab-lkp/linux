// SPDX-License-Identifier: GPL-2.0
/*
 * BTRFS acomp layer
 *
 * Copyright (c) 2025, Intel Corporation
 * Author: Giovanni Cabiddu <giovanni.cabiddu@intel.com>
 */
#ifdef CONFIG_BTRFS_EXPERIMENTAL
#include <crypto/acompress.h>
#include <linux/bio.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/rtnetlink.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "compression.h"
#include "acomp_workspace.h"

static int folios_to_scatterlist(struct folio **folios, unsigned int nr_folios,
				 size_t size, struct scatterlist *sg,
				 unsigned int first_folio_offset)
{
	size_t available, len;
	unsigned int offset;
	int i;

	if (!folios || nr_folios == 0 || !sg)
		return -EINVAL;

	if (nr_folios > BTRFS_ACOMP_MAX_SGL_ENTRIES)
		return -E2BIG;

	sg_init_table(sg, nr_folios);

	for (i = 0; i < nr_folios && size; i++) {
		/* For the first folio, use the provided offset; for others, use 0 */
		offset = (i == 0) ? first_folio_offset : 0;
		available = folio_size(folios[i]) - offset;

		len = min(size, available);
		sg_set_folio(&sg[i], folios[i], len, offset);
		size -= len;
	}

	return 0;
}

static int build_acomp_attr_buffer(u8 *buf, unsigned int *len, u32 level)
{
	unsigned int total_len;
	struct rtattr *rta;
	u8 *pos;

	if (!buf || !len || *len == 0)
		return -EINVAL;

	total_len = RTA_SPACE(sizeof(u32)) + RTA_SPACE(0);
	if (total_len > *len)
		return -E2BIG;

	pos = buf;

	rta = (struct rtattr *)pos;
	rta->rta_type = CRYPTO_COMP_PARAM_LEVEL;
	rta->rta_len = RTA_LENGTH(sizeof(u32));
	memcpy(RTA_DATA(rta), &level, sizeof(level));
	pos += RTA_SPACE(sizeof(u32));

	rta = (struct rtattr *)pos;
	rta->rta_type = CRYPTO_COMP_PARAM_LAST;
	rta->rta_len = RTA_LENGTH(0);
	pos += RTA_SPACE(0);

	*len = total_len;

	return 0;
}

int acomp_comp_folios(struct btrfs_acomp_workspace *acomp_ws,
		      struct btrfs_fs_info *fs_info,
		      struct address_space *mapping, u64 start, unsigned long len,
		      struct folio **folios, unsigned long *out_folios,
		      unsigned long *total_in, unsigned long *total_out, int level)
{
	struct scatterlist *out_sgl = NULL;
	struct scatterlist *in_sgl = NULL;
	const u64 orig_end = start + len;
	struct crypto_acomp *tfm = NULL;
	struct folio **in_folios = NULL;
	unsigned int first_folio_offset;
	unsigned int nr_dst_folios = 0;
	struct folio *out_folio = NULL;
	unsigned int nr_src_folios = 0;
	struct acomp_req *req = NULL;
	unsigned int nr_folios = 0;
	unsigned int dst_size = 0;
	unsigned int raw_attr_len;
	unsigned int bytes_left;
	unsigned int nofs_flags;
	struct crypto_wait wait;
	struct folio *in_folio;
	unsigned int cur_len;
	unsigned int i;
	u64 cur_start;
	u8 *raw_attr;
	int ret;

	if (!acomp_ws)
		return -EOPNOTSUPP;

	/* Check if offload is enabled and acquire reference */
	if (!atomic_read(&fs_info->compress_offload_enabled))
		return -EOPNOTSUPP;

	if (!atomic_inc_not_zero(&fs_info->compr_resource_refcnt))
		return -EOPNOTSUPP;

	/* Protect against GFP_KERNEL allocations in crypto subsystem */
	nofs_flags = memalloc_nofs_save();

	in_folios = btrfs_acomp_get_folios(acomp_ws);
	if (!in_folios) {
		btrfs_err(fs_info, "No input folios in workspace\n");
		ret = -EINVAL;
		goto out;
	}

	cur_start = start;
	while (cur_start < orig_end && nr_src_folios < BTRFS_ACOMP_MAX_SGL_ENTRIES) {
		ret = btrfs_compress_filemap_get_folio(mapping, cur_start, &in_folio);
		if (ret) {
			btrfs_err(fs_info, "Error %d getting folio at %llu\n", ret, cur_start);
			goto out;
		}

		cur_len = btrfs_calc_input_length(in_folio, orig_end, cur_start);
		cur_start += cur_len;

		in_folios[nr_src_folios] = in_folio;
		nr_src_folios++;
	}

	/* Check if we can allocate enough output folios */
	if (nr_src_folios > *out_folios) {
		btrfs_err(fs_info, "Not enough output folios: have %lu need %u\n",
			  *out_folios, nr_src_folios);
		ret = -E2BIG;
		goto out;
	}

	do {
		out_folio = btrfs_alloc_compr_folio(fs_info);
		if (!out_folio) {
			btrfs_err(fs_info, "Failed to allocate output folio %u\n",
				  nr_dst_folios);
			ret = -ENOMEM;
			goto out;
		}

		folios[nr_dst_folios] = out_folio;
		nr_dst_folios++;
		dst_size += folio_size(out_folio);
	} while (dst_size < len && nr_dst_folios < BTRFS_ACOMP_MAX_SGL_ENTRIES);

	in_sgl = btrfs_acomp_get_input_sgl(acomp_ws);
	if (!in_sgl) {
		ret = -EINVAL;
		goto out;
	}

	/* Calculate the offset within the first input folio */
	first_folio_offset = offset_in_folio(in_folios[0], start);

	ret = folios_to_scatterlist(in_folios, nr_src_folios, len, in_sgl, first_folio_offset);
	if (ret) {
		btrfs_err(fs_info, "Failed to build input scatterlist\n");
		goto out;
	}

	out_sgl = btrfs_acomp_get_output_sgl(acomp_ws);
	if (!out_sgl) {
		ret = -EINVAL;
		goto out;
	}

	ret = folios_to_scatterlist(folios, nr_dst_folios, dst_size, out_sgl, 0);
	if (ret) {
		btrfs_err(fs_info, "Failed to build output scatterlist\n");
		goto out;
	}

	crypto_init_wait(&wait);

	/* Get pre-allocated tfm and request from workspace */
	tfm = btrfs_acomp_get_tfm(acomp_ws);
	req = btrfs_acomp_get_request(acomp_ws);
	if (!tfm || !req) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	raw_attr = btrfs_acomp_get_attr_buffer(acomp_ws);
	raw_attr_len = BTRFS_ACOMP_ATTR_BUF_SIZE;
	ret = build_acomp_attr_buffer(raw_attr, &raw_attr_len, level);
	if (ret) {
		btrfs_err(fs_info, "Failed to build acomp attr buffer: %d\n", ret);
		goto out;
	}

	ret = crypto_acomp_setparam(tfm, raw_attr, raw_attr_len);
	if (ret) {
		btrfs_err(fs_info, "Failed to set acomp params: %d\n", ret);
		goto out;
	}

	acomp_request_set_params(req, in_sgl, out_sgl, len, dst_size);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);

	ret = crypto_wait_req(crypto_acomp_compress(req), &wait);
	if (ret)
		goto out;

	*total_in = len;
	*total_out = req->dlen;

	/* Calculate number of folios used based on total_out */
	bytes_left = *total_out;
	for (i = 0, nr_folios = 0; i < nr_dst_folios && bytes_left > 0; i++) {
		bytes_left -= min_t(size_t, bytes_left, folio_size(folios[i]));
		nr_folios++;
	}

out:
	/* Free out un-used folios (or all on error since nr_folios = 0) */
	for (i = nr_folios; i < nr_dst_folios; i++) {
		if (folios[i]) {
			btrfs_free_compr_folio(folios[i]);
			folios[i] = NULL;
		}
	}

	/* Free input folios */
	for (i = 0; i < nr_src_folios; i++)
		if (in_folios[i]) {
			folio_put(in_folios[i]);
			in_folios[i] = NULL;
		}

	*out_folios = nr_folios;

	memalloc_nofs_restore(nofs_flags);

	/* Release reference and wake up any waiters */
	if (atomic_dec_and_test(&fs_info->compr_resource_refcnt))
		wake_up(&fs_info->compr_wait_queue);

	return ret;
}

int acomp_decomp_bio(struct btrfs_acomp_workspace *acomp_ws,
		     struct btrfs_fs_info *fs_info,
		     struct folio **in_folios,
		     struct compressed_bio *cb, size_t srclen,
		     unsigned long total_folios_in)
{
	const u32 min_folio_size = btrfs_min_folio_size(fs_info);
	unsigned int nr_dst_folios = BTRFS_MAX_COMPRESSED_PAGES;
	struct scatterlist *out_sgl = NULL;
	struct scatterlist *in_sgl = NULL;
	struct folio **out_folios = NULL;
	struct crypto_acomp *tfm = NULL;
	struct acomp_req *req = NULL;
	struct crypto_wait wait;
	unsigned int nofs_flags;
	unsigned int dst_size;
	char *data_out = NULL;
	int bytes_left = 0;
	unsigned int i;
	int ret, ret2;

	if (!acomp_ws)
		return -EOPNOTSUPP;

	/* Check if offload is enabled and acquire reference */
	if (!atomic_read(&fs_info->compress_offload_enabled))
		return -EOPNOTSUPP;

	if (!atomic_inc_not_zero(&fs_info->compr_resource_refcnt))
		return -EOPNOTSUPP;

	/* Protect against GFP_KERNEL allocations in crypto subsystem */
	nofs_flags = memalloc_nofs_save();

	in_sgl = btrfs_acomp_get_input_sgl(acomp_ws);
	if (!in_sgl) {
		ret = -EINVAL;
		goto out;
	}

	out_sgl = btrfs_acomp_get_output_sgl(acomp_ws);
	if (!out_sgl) {
		ret = -EINVAL;
		goto out;
	}

	out_folios = btrfs_acomp_get_folios(acomp_ws);
	if (!out_folios) {
		ret = -EINVAL;
		goto out;
	}

	ret = folios_to_scatterlist(in_folios, total_folios_in, srclen, in_sgl, 0);
	if (ret)
		goto out;

	for (i = 0; i < nr_dst_folios; i++) {
		out_folios[i] = btrfs_alloc_compr_folio(fs_info);
		if (!out_folios[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	dst_size = nr_dst_folios * min_folio_size;

	ret = folios_to_scatterlist(out_folios, nr_dst_folios, dst_size, out_sgl, 0);
	if (ret)
		goto out;

	crypto_init_wait(&wait);

	/* Get pre-allocated tfm and request from workspace */
	tfm = btrfs_acomp_get_tfm(acomp_ws);
	req = btrfs_acomp_get_request(acomp_ws);
	if (!tfm || !req) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	acomp_request_set_params(req, in_sgl, out_sgl, srclen, dst_size);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);

	ret = crypto_wait_req(crypto_acomp_decompress(req), &wait);
	if (ret)
		goto out;

	bytes_left = req->dlen;
	for (i = 0; i < nr_dst_folios && bytes_left > 0; i++) {
		size_t folio_bytes = min_t(size_t, bytes_left, min_folio_size);
		unsigned long buf_start = req->dlen - bytes_left;

		data_out = kmap_local_folio(out_folios[i], 0);

		ret2 = btrfs_decompress_buf2page(data_out, folio_bytes, cb, buf_start);
		kunmap_local(data_out);

		if (ret2 == 0) {
			ret = 0;
			goto out;
		}

		bytes_left -= folio_bytes;
	}

out:
	if (out_folios) {
		for (i = 0; i < nr_dst_folios; i++) {
			if (out_folios[i]) {
				folio_put(out_folios[i]);
				out_folios[i] = NULL;
			}
		}
	}

	memalloc_nofs_restore(nofs_flags);

	/* Release reference and wake up any waiters */
	if (atomic_dec_and_test(&fs_info->compr_resource_refcnt))
		wake_up(&fs_info->compr_wait_queue);

	return ret;
}

static const char *zlib_acomp_alg_name = "qat_zlib_deflate";
static const char *zstd_acomp_alg_name = "qat_zstd";

bool acomp_has_zlib(void)
{
	return crypto_has_acomp(zlib_acomp_alg_name, 0, 0);
}

bool acomp_has_zstd(void)
{
	return crypto_has_acomp(zstd_acomp_alg_name, 0, 0);
}

static struct btrfs_acomp_workspace *acomp_workspace_alloc(struct btrfs_fs_info *fs_info,
							   const char *alg_name)
{
	struct btrfs_acomp_workspace *acomp_ws;

	if (!alg_name)
		return NULL;

	if (!crypto_has_acomp(alg_name, 0, 0))
		return NULL;

	/* Only allocate workspace if offload is enabled */
	if (!fs_info || !atomic_read(&fs_info->compress_offload_enabled))
		return NULL;

	acomp_ws = kzalloc(sizeof(*acomp_ws), GFP_KERNEL);
	if (!acomp_ws)
		return NULL;

	sg_init_table(acomp_ws->in_sgl, BTRFS_ACOMP_MAX_SGL_ENTRIES);
	sg_init_table(acomp_ws->out_sgl, BTRFS_ACOMP_MAX_SGL_ENTRIES);

	acomp_ws->alg_name = alg_name;
	acomp_ws->attr_buf_len = BTRFS_ACOMP_ATTR_BUF_SIZE;

	/* Allocate tfm and req */
	acomp_ws->tfm = crypto_alloc_acomp(alg_name, 0, 0);
	if (IS_ERR(acomp_ws->tfm)) {
		btrfs_err(fs_info, "Failed to allocate acomp tfm for %s: %ld\n",
			  alg_name, PTR_ERR(acomp_ws->tfm));
		kfree(acomp_ws);
		return NULL;
	}

	acomp_ws->req = acomp_request_alloc(acomp_ws->tfm);
	if (!acomp_ws->req) {
		btrfs_err(fs_info, "Failed to allocate acomp request for %s\n", alg_name);
		crypto_free_acomp(acomp_ws->tfm);
		kfree(acomp_ws);
		return NULL;
	}

	return acomp_ws;
}

struct btrfs_acomp_workspace *acomp_zlib_workspace_alloc(struct btrfs_fs_info *fs_info)
{
	return acomp_workspace_alloc(fs_info, zlib_acomp_alg_name);
}

struct btrfs_acomp_workspace *acomp_zstd_workspace_alloc(struct btrfs_fs_info *fs_info)
{
	return acomp_workspace_alloc(fs_info, zstd_acomp_alg_name);
}

void acomp_workspace_free(struct btrfs_acomp_workspace *acomp_ws)
{
	if (acomp_ws) {
		if (acomp_ws->req)
			acomp_request_free(acomp_ws->req);
		if (acomp_ws->tfm)
			crypto_free_acomp(acomp_ws->tfm);
	}
	kfree(acomp_ws);
}
#endif /* CONFIG_BTRFS_EXPERIMENTAL */
