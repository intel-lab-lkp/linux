// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/scatterlist.h>
#include <crypto/acompress.h>

#include "compress.h"

static int z_erofs_crypto_decompress_mem(struct z_erofs_decompress_req *rq,
				struct crypto_acomp *erofs_tfm)
{
	unsigned int nrpages_out = rq->outpages, nrpages_in = rq->inpages;
	struct sg_table st_src, st_dst;
	struct scatterlist *sg_src, *sg_dst;
	struct acomp_req *req;
	struct crypto_wait wait;
	int ret, i;
	u8 *headpage;


	WARN_ON(!*rq->in);
	headpage = kmap_local_page(*rq->in);
	ret = z_erofs_fixup_insize(rq, headpage + rq->pageofs_in,
				min_t(unsigned int, rq->inputsize,
							rq->sb->s_blocksize - rq->pageofs_in));
	kunmap_local(headpage);
	if (ret)
		return ret;

	req = acomp_request_alloc(erofs_tfm);
	if (!req) {
		erofs_err(rq->sb, "failed to alloc decompress request");
		return -ENOMEM;
	}

	if (sg_alloc_table(&st_src, nrpages_in, GFP_KERNEL)) {
		acomp_request_free(req);
		return -ENOMEM;
	}

	if (sg_alloc_table(&st_dst, nrpages_out, GFP_KERNEL)) {
		acomp_request_free(req);
		sg_free_table(&st_src);
		return -ENOMEM;
	}

	for_each_sg(st_src.sgl, sg_src, nrpages_in, i) {
		if (i == 0)
			sg_set_page(sg_src, rq->in[0],
				PAGE_SIZE - rq->pageofs_in, rq->pageofs_in);
		else if (i == nrpages_in - 1)
			sg_set_page(sg_src, rq->in[i],
				rq->pageofs_in + rq->inputsize - (nrpages_in - 1) * PAGE_SIZE, 0);
		else
			sg_set_page(sg_src, rq->in[i], PAGE_SIZE, 0);
	}

	i = 0;
	for_each_sg(st_dst.sgl, sg_dst, nrpages_out, i) {
		if (i == 0)
			sg_set_page(sg_dst, rq->out[0],
				PAGE_SIZE - rq->pageofs_out, rq->pageofs_out);
		else if (i == nrpages_out)
			sg_set_page(sg_dst, rq->out[i],
				rq->pageofs_out + rq->outputsize - (nrpages_out - 1) * PAGE_SIZE, 0);
		else
			sg_set_page(sg_dst, rq->out[i], PAGE_SIZE, 0);
	}

	acomp_request_set_params(req, st_src.sgl,
		st_dst.sgl, rq->inputsize, rq->outputsize);

	crypto_init_wait(&wait);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				crypto_req_done, &wait);

	ret = crypto_wait_req(crypto_acomp_decompress(req), &wait);
	if (ret < 0) {
		erofs_err(rq->sb, "failed to decompress %d in[%u, %u] out[%u]",
					ret, rq->inputsize, rq->pageofs_in, rq->outputsize);
		ret = -EIO;
	} else
		ret = 0;

	acomp_request_free(req);
	sg_free_table(&st_src);
	sg_free_table(&st_dst);
	return ret;
}

int z_erofs_crypto_decompress(struct z_erofs_decompress_req *rq,
				struct crypto_acomp *erofs_tfm, struct page **pgpl)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	int i;

	/* one optimized fast path only for non bigpcluster cases yet */
	if (rq->inputsize <= PAGE_SIZE && nrpages_out == 1 && !rq->inplace_io) {
		WARN_ON(!*rq->out);
		goto dstmap_out;
	}

	for (i = 0; i < rq->outpages; i++) {
		struct page *const page = rq->out[i];
		struct page *victim;

		if (!page) {
			victim = __erofs_allocpage(pgpl, rq->gfp, true);
			if (!victim)
				return -ENOMEM;
			set_page_private(victim, Z_EROFS_SHORTLIVED_PAGE);
			rq->out[i] = victim;
		}
	}

dstmap_out:
	return z_erofs_crypto_decompress_mem(rq, erofs_tfm);
}

struct crypto_acomp *z_erofs_crypto_get_enbale_engine(int type)
{
	int i = 0;

	while (z_erofs_crypto[type][i].crypto_name) {
		if (z_erofs_crypto[type][i].enabled)
			return z_erofs_crypto[type][i].erofs_tfm;
		i++;
	}

	return NULL;
}

static int z_erofs_crypto_get_compress_type(const char *name)
{
	int i, j;

	for (i = 0; i < Z_EROFS_COMPRESSION_MAX; i++) {
		j = 0;
		while (z_erofs_crypto[i][j].crypto_name) {
			if (!strncmp(name, z_erofs_crypto[i][j].crypto_name,
					strlen(z_erofs_crypto[i][j].crypto_name))) {
				return i;
			}
			j++;
		}
	}

	return -EINVAL;
}

int z_erofs_crypto_enable_engine(const char *name)
{
	int i = 0, type;

	type = z_erofs_crypto_get_compress_type(name);
	if (type < 0)
		return -EINVAL;

	while (z_erofs_crypto[type][i].crypto_name) {
		if (!strncmp(z_erofs_crypto[type][i].crypto_name, name,
				strlen(z_erofs_crypto[type][i].crypto_name))) {
			if (z_erofs_crypto[type][i].enabled)
				break;

			z_erofs_crypto[type][i].erofs_tfm =
				crypto_alloc_acomp(z_erofs_crypto[type][i].crypto_name, 0, 0);
			if (IS_ERR(z_erofs_crypto[type][i].erofs_tfm)) {
				z_erofs_crypto[type][i].erofs_tfm = NULL;
				break;
			}
			z_erofs_crypto[type][i].enabled = true;
		} else if (z_erofs_crypto[type][i].enabled) {
			if (z_erofs_crypto[type][i].erofs_tfm)
				crypto_free_acomp(z_erofs_crypto[type][i].erofs_tfm);
			z_erofs_crypto[type][i].enabled = false;
		}
		i++;
	}

	return 0;
}

int z_erofs_crypto_disable_engine(const char *name)
{
	int i = 0, type;

	type = z_erofs_crypto_get_compress_type(name);
	if (type < 0)
		return -EINVAL;

	while (z_erofs_crypto[type][i].crypto_name) {
		if (!strncmp(z_erofs_crypto[type][i].crypto_name, name,
				strlen(z_erofs_crypto[type][i].crypto_name))) {
			if (z_erofs_crypto[type][i].enabled &&
					z_erofs_crypto[type][i].erofs_tfm) {
				crypto_free_acomp(z_erofs_crypto[type][i].erofs_tfm);
				z_erofs_crypto[type][i].erofs_tfm = NULL;
				z_erofs_crypto[type][i].enabled = false;
			}
		}
		i++;
	}

	return 0;

}

struct z_erofs_crypto_engine *z_erofs_crypto[] = {
	[Z_EROFS_COMPRESSION_LZ4] = &(struct z_erofs_crypto_engine) {NULL},
	[Z_EROFS_COMPRESSION_LZMA] = &(struct z_erofs_crypto_engine) {NULL},
	[Z_EROFS_COMPRESSION_DEFLATE] = {&(struct z_erofs_crypto_engine) {
			.crypto_name = "qat_deflate",
			.enabled = false,
			.erofs_tfm = NULL,
		},
		&(const struct z_erofs_crypto_engine) { NULL },
	},
	[Z_EROFS_COMPRESSION_ZSTD] = &(struct z_erofs_crypto_engine) {NULL},
};
