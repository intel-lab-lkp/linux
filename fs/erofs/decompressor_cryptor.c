// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2024 Amlogic, Inc. All rights reserved.
 */
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/xz.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include "compress.h"
#include "internal.h"

static int crypto_max_distance_pages;

int z_erofs_load_crypto_config(struct super_block *sb,
			       struct erofs_super_block *dsb,
			       void *data, int size)
{
	struct erofs_sb_info *sbi;
	struct z_erofs_crypto_cfgs *crypto = (struct z_erofs_crypto_cfgs *)data;
	int max_pages;

	sbi = EROFS_SB(sb);
	if (!sbi)
		return -EINVAL;

	if (sbi->crypto) {
		erofs_err(sb, "already have crypto\n");
		return -EINVAL;
	}
	if (crypto) {
		max_pages = BIT(crypto->max_distance) / PAGE_SIZE;
		if (max_pages > CONFIG_EROFS_CRYPTO_MAX_DISTANCE_PAGES) {
			erofs_err(sb, "bad max distance:%d\n", max_pages);
			return -EINVAL;
		}

		if (max_pages > crypto_max_distance_pages)
			crypto_max_distance_pages = max_pages;
		sbi->crypto = crypto_alloc_comp(crypto->crypto_name, 0, 0);
		if (IS_ERR(sbi->crypto)) {
			erofs_err(sb, "failed to alloc cryto %s\n",
				  crypto->crypto_name);
			return PTR_ERR(sbi->crypto);
		}
		erofs_info(sb, "max pcluster:%d, distance:%d, %d, crypto:%s\n",
			   crypto->max_pclusterblks,
			   crypto->max_distance, crypto_max_distance_pages,
			   crypto->crypto_name);
		return z_erofs_gbuf_growsize(crypto_max_distance_pages);
	} else {
		return -EINVAL;
	}
}

static void *z_erofs_crypto_handle_inplace_io(struct z_erofs_decompress_req *rq,
					      void *inpage,
					      unsigned int *inputmargin,
					      int *maptype,
					      bool support_0padding)
{
	unsigned int nrpages_in, nrpages_out;
	unsigned int ofull, oend, inputsize, total, i, j;
	struct page **in;
	void *src, *tmp;

	inputsize = rq->inputsize;
	nrpages_in = PAGE_ALIGN(inputsize) >> PAGE_SHIFT;
	oend = rq->pageofs_out + rq->outputsize;
	ofull = PAGE_ALIGN(oend);
	nrpages_out = ofull >> PAGE_SHIFT;

	if (rq->inplace_io) {
		if (rq->partial_decoding)
			goto docopy;

		for (i = 0; i < nrpages_in; ++i) {
			WARN_ON(!rq->in[i]);
			for (j = 0; j < nrpages_out; ++j) {
				if (rq->out[j] == rq->in[i])
					goto docopy;
			}
		}
	}

	if (nrpages_in <= 1) {
		*maptype = 0;
		return inpage;
	}
	kunmap_local(inpage);
	might_sleep();
	src = erofs_vm_map_ram(rq->in, nrpages_in);
	if (!src)
		return ERR_PTR(-ENOMEM);
	*maptype = 1;
	return src;

docopy:
	/* Or copy compressed data which can be overlapped to per-CPU buffer */
	in = rq->in;
	src = z_erofs_get_gbuf(nrpages_in);
	if (!src) {
		WARN_ON(1);
		kunmap_local(inpage);
		return ERR_PTR(-EFAULT);
	}

	tmp = src;
	total = rq->inputsize;
	while (total) {
		unsigned int page_copycnt =
			min_t(unsigned int, total, PAGE_SIZE - *inputmargin);

		if (!inpage)
			inpage = kmap_local_page(*in);
		memcpy(tmp, inpage + *inputmargin, page_copycnt);
		kunmap_local(inpage);
		inpage = NULL;
		tmp += page_copycnt;
		total -= page_copycnt;
		++in;
		*inputmargin = 0;
	}
	*maptype = 2;
	return src;
}

static int z_erofs_crypto_decompress_mem(struct z_erofs_decompress_req *rq, u8 *out)
{
	unsigned int inputmargin;
	u8 *src, *headpage;
	int ret;
	int maptype;
	bool support_0padding = true;
	struct erofs_sb_info *sbi;
	int outsize;

	WARN_ON(!*rq->in);
	inputmargin = 0;
	headpage = kmap_local_page(*rq->in);

	sbi = EROFS_SB(rq->sb);

	/* decompression inplace is only safe when 0padding is enabled */
	if (erofs_sb_has_zero_padding(EROFS_SB(rq->sb))) {
		support_0padding = true;

		/* skip zero padding at beginning of page */
		while (!headpage[inputmargin & ~PAGE_MASK])
			if (!(++inputmargin & ~PAGE_MASK))
				break;

		if (inputmargin >= rq->inputsize) {
			kunmap_local(headpage);
			return -EIO;
		}
	}

	rq->inputsize -= inputmargin;
	src = z_erofs_crypto_handle_inplace_io(rq, headpage, &inputmargin,
					       &maptype, support_0padding);
	if (IS_ERR(src))
		return PTR_ERR(src);

	pr_debug("%s, type:%d, in:%p, out:%p, in page:%lx, out_page:%lx\n",
		 __func__, maptype, src, out,
		 page_to_pfn(*rq->in), page_to_pfn(*rq->out));

	outsize = rq->outputsize;
	ret = crypto_comp_decompress(sbi->crypto, src + inputmargin,
				     rq->inputsize, out, &rq->outputsize);
	if (ret < 0 || outsize != rq->outputsize) {
		erofs_err(rq->sb, "failed to decompress %d in[%u, %u] out[%u]",
			  ret, rq->inputsize, inputmargin, rq->outputsize);

		print_hex_dump_debug("[ in]: ", DUMP_PREFIX_OFFSET,
				     16, 1, src + inputmargin,
				     rq->inputsize, true);
		print_hex_dump_debug("[out]: ", DUMP_PREFIX_OFFSET,
				     16, 1, out, rq->outputsize, true);

		if (ret >= 0)
			memset(out + ret, 0, rq->outputsize - ret);
		ret = -EIO;
	} else {
		ret = 0;
	}

	if (maptype == 0) {
		kunmap_local(src);
	} else if (maptype == 1) {
		vm_unmap_ram(src, PAGE_ALIGN(rq->inputsize) >> PAGE_SHIFT);
	} else if (maptype == 2) {
		z_erofs_put_gbuf(src);
	} else {
		WARN_ON(1);
		return -EFAULT;
	}
	return ret;
}

/*
 * Fill all gaps with bounce pages if it's a sparse page list. Also check if
 * all physical pages are consecutive, which can be seen for moderate CR.
 */
static int z_erofs_crypto_prepare_dstpages(struct z_erofs_decompress_req *rq,
					   struct page **pagepool)
{
	const unsigned int nr = PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	struct page *available[CONFIG_EROFS_CRYPTO_MAX_DISTANCE_PAGES] = { NULL };
	unsigned long bounced[DIV_ROUND_UP(CONFIG_EROFS_CRYPTO_MAX_DISTANCE_PAGES, BITS_PER_LONG)];
	void *kaddr = NULL;
	unsigned int i, j, top;

	memset(bounced, 0, sizeof(bounced));
	top = 0;
	for (i = j = 0; i < nr; ++i, ++j) {
		struct page *const page = rq->out[i];
		struct page *victim;

		if (j >= crypto_max_distance_pages)
			j = 0;

		/* 'valid' bounced can only be tested after a complete round */
		if (test_bit(j, bounced)) {
			WARN_ON(i < crypto_max_distance_pages);
			WARN_ON(top >= crypto_max_distance_pages);
			available[top++] = rq->out[i - crypto_max_distance_pages];
		}

		if (page) {
			__clear_bit(j, bounced);
			if (!PageHighMem(page)) {
				if (!i) {
					kaddr = page_address(page);
					continue;
				}
				if (kaddr &&
				    kaddr + PAGE_SIZE == page_address(page)) {
					kaddr += PAGE_SIZE;
					continue;
				}
			}
			kaddr = NULL;
			continue;
		}
		kaddr = NULL;
		__set_bit(j, bounced);

		if (top) {
			victim = available[--top];
			get_page(victim);
		} else {
			victim = erofs_allocpage(pagepool,
						 GFP_KERNEL | __GFP_NOFAIL);
			set_page_private(victim, Z_EROFS_SHORTLIVED_PAGE);
		}
		rq->out[i] = victim;
	}
	return kaddr ? 1 : 0;
}

int z_erofs_crypto_decompress(struct z_erofs_decompress_req *rq,
			      struct page **pagepool)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	unsigned int dst_maptype;
	void *dst;
	int ret;

	/* one optimized fast path only for non bigpcluster cases yet */
	if (rq->inputsize <= PAGE_SIZE && nrpages_out == 1 && !rq->inplace_io) {
		WARN_ON(!*rq->out);
		dst = kmap_local_page(*rq->out);
		dst_maptype = 0;
		goto dstmap_out;
	}

	ret = z_erofs_crypto_prepare_dstpages(rq, pagepool);
	if (ret < 0)
		return ret;

	dst = erofs_vm_map_ram(rq->out, nrpages_out);
	if (!dst)
		return -ENOMEM;
	dst_maptype = 2;

dstmap_out:
	pr_debug("%s, dst:%p, map:%d, pageofs_out:%d, outputsize:%d, inputsize:%d, inplace_io:%d, partial:%d\n",
		 __func__, dst, dst_maptype,
		 rq->pageofs_out, rq->outputsize, rq->inputsize,
		 rq->inplace_io, rq->partial_decoding);

	ret = z_erofs_crypto_decompress_mem(rq, dst + rq->pageofs_out);

	if (!dst_maptype)
		kunmap_local(dst);
	else if (dst_maptype == 2)
		vm_unmap_ram(dst, nrpages_out);

	return ret;
}
