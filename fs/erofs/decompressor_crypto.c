// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/scatterlist.h>
#include <crypto/acompress.h>

#include "compress.h"

static int z_erofs_crypto_decompress_mem(struct z_erofs_decompress_req *rq,
				struct crypto_acomp *erofs_tfm)
{
	struct sg_table st_src, st_dst;
	struct acomp_req *req;
	struct crypto_wait wait;
	u8 *headpage;
	int ret;

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

	ret = sg_alloc_table_from_pages_segment(&st_src,
					rq->in, rq->inpages, rq->pageofs_in,
					rq->inputsize, UINT_MAX, GFP_KERNEL);
	if (ret < 0)
		goto failed_src_alloc;

	ret = sg_alloc_table_from_pages_segment(&st_dst,
					rq->out, rq->outpages, rq->pageofs_out,
					rq->outputsize, UINT_MAX, GFP_KERNEL);
	if (ret < 0)
		goto failed_dst_alloc;

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

	sg_free_table(&st_dst);
failed_dst_alloc:
	sg_free_table(&st_src);
failed_src_alloc:
	acomp_request_free(req);
	return ret;
}

int z_erofs_crypto_decompress(struct z_erofs_decompress_req *rq,
				struct crypto_acomp *erofs_tfm, struct page **pgpl)
{
	int i;

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

	return z_erofs_crypto_decompress_mem(rq, erofs_tfm);
}

struct crypto_acomp *z_erofs_crypto_get_engine(int type)
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
			if (!strcmp(name, z_erofs_crypto[i][j].crypto_name))
				return i;

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
		if (!strcmp(z_erofs_crypto[type][i].crypto_name, name)) {
			if (z_erofs_crypto[type][i].enabled)
				break;

			z_erofs_crypto[type][i].erofs_tfm =
				crypto_alloc_acomp(z_erofs_crypto[type][i].crypto_name, 0, 0);
			if (IS_ERR(z_erofs_crypto[type][i].erofs_tfm)) {
				z_erofs_crypto[type][i].erofs_tfm = NULL;
				return -ENXIO;
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

void z_erofs_crypto_disable_engine(void)
{
	int i = 0, type;

	for (type = 0; type < Z_EROFS_COMPRESSION_MAX; type++) {
		i = 0;
		while (z_erofs_crypto[type][i].crypto_name) {
			if (z_erofs_crypto[type][i].enabled &&
					z_erofs_crypto[type][i].erofs_tfm) {
				crypto_free_acomp(z_erofs_crypto[type][i].erofs_tfm);
				z_erofs_crypto[type][i].erofs_tfm = NULL;
				z_erofs_crypto[type][i].enabled = false;
			}
			i++;
		}
	}
}

int z_erofs_crypto_engine_format(char *buf)
{
	int type, i, len = 0;

	for (type = 0; type < Z_EROFS_COMPRESSION_MAX; type++) {
		i = 0;
		while (z_erofs_crypto[type][i].crypto_name) {
			if (z_erofs_crypto[type][i].enabled)
				len += scnprintf(buf + len, PATH_MAX - len, "%s ",
							z_erofs_crypto[type][i].crypto_name);
			i++;
		}
	}
	return len;
}

struct z_erofs_crypto_engine z_erofs_crypto[Z_EROFS_COMPRESSION_MAX][2] = {
	[Z_EROFS_COMPRESSION_LZ4] = {
		(struct z_erofs_crypto_engine) {NULL},
		(struct z_erofs_crypto_engine) {NULL},
	},
	[Z_EROFS_COMPRESSION_LZMA] = {
		(struct z_erofs_crypto_engine) {NULL},
		(struct z_erofs_crypto_engine) {NULL},
	},
	[Z_EROFS_COMPRESSION_DEFLATE] = {
		(struct z_erofs_crypto_engine) {
			.crypto_name = "qat_deflate",
			.enabled = false,
			.erofs_tfm = NULL,
		},
		(struct z_erofs_crypto_engine) {NULL},
	},
	[Z_EROFS_COMPRESSION_ZSTD] = {
		(struct z_erofs_crypto_engine) {NULL},
		(struct z_erofs_crypto_engine) {NULL},
	},
};
