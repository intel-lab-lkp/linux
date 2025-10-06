/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation. */

#include <linux/range.h>
#include <cxlmem.h>
#include <cxl.h>

#include "platform_quirks.h"
#include "core.h"

/* Start of CFMWS range that end before x86 Low Memory Holes */
#define LMH_CFMWS_RANGE_START 0x0ULL

/**
 * __platform_cxlrd_matches_cxled() - Platform quirk to match CXL Root and
 * Endpoint Decoders. It allows matching on platforms with LMH's.
 * @cxlrd: The Root Decoder against which @cxled is tested for matching.
 * @cxled: The Endpoint Decoder to be tested for matching @cxlrd.
 *
 * __platform_cxlrd_matches_cxled() is typically called from the
 * match_*_by_range() functions in region.c. It checks if an endpoint decoder
 * matches a given root decoder and returns true to allow the driver to succeed
 * in the construction of regions where it would otherwise fail for the presence
 * of a Low Memory Hole (see Documentation/driver-api/cxl/conventions.rst).
 *
 * In x86 platforms with LMH's, the CFMWS ranges never intersect the LMH, the
 * endpoint decoder's HPA range size is always guaranteed aligned to NIW*256MB
 * and also typically larger than the matching root decoder's, and the root
 * decoder's range end is at an address that is necessarily less than SZ_4G
 * (i.e., the Hole is in Low Memory - this function doesn't deal with other
 * kinds of holes).
 *
 * Return: true if an endpoint matches a root decoder, else false.
 */
bool __platform_cxlrd_matches_cxled(const struct cxl_root_decoder *cxlrd,
				    const struct cxl_endpoint_decoder *cxled)
{
	const struct range *rd_r, *sd_r;
	int align;

	rd_r = &cxlrd->cxlsd.cxld.hpa_range;
	sd_r = &cxled->cxld.hpa_range;
	align = cxled->cxld.interleave_ways * SZ_256M;

	if (rd_r->start == LMH_CFMWS_RANGE_START &&
	    rd_r->start == sd_r->start && rd_r->end < sd_r->end &&
	    rd_r->end < (LMH_CFMWS_RANGE_START + SZ_4G) &&
	    IS_ALIGNED(range_len(sd_r), align))
		return true;

	return false;
}
EXPORT_SYMBOL_NS_GPL(__platform_cxlrd_matches_cxled, "CXL");

/**
 * __platform_region_matches_cxld() - Platform quirk to match a CXL Region and a
 * Switch or Endpoint Decoder. It allows matching on platforms with LMH's.
 * @p: Region Params against which @cxled is matched.
 * @cxld: Switch or Endpoint Decoder to be tested for matching @p.
 *
 * Similar to platform_cxlrd_matches_cxled(), it matches regions and
 * decoders on platforms with LMH's.
 *
 * Return: true if a Decoder matches a Region, else false.
 */
bool __platform_region_matches_cxld(const struct cxl_region_params *p,
				    const struct cxl_decoder *cxld)
{
	const struct range *r = &cxld->hpa_range;
	const struct resource *res = p->res;
	int align = cxld->interleave_ways * SZ_256M;

	if (res->start == LMH_CFMWS_RANGE_START && res->start == r->start &&
	    res->end < r->end && res->end < (LMH_CFMWS_RANGE_START + SZ_4G) &&
	    IS_ALIGNED(range_len(r), align))
		return true;

	return false;
}
EXPORT_SYMBOL_NS_GPL(__platform_region_matches_cxld, "CXL");

void platform_res_adjust(struct resource *res,
			 struct cxl_endpoint_decoder *cxled,
			 const struct cxl_root_decoder *cxlrd)
{
	if (!platform_cxlrd_matches_cxled(cxlrd, cxled))
		return;

	guard(rwsem_write)(&cxl_rwsem.dpa);
	dev_dbg(cxled_to_memdev(cxled)->dev.parent,
		"Low Memory Hole detected. Resources were (%s: %pr, %pr)\n",
		dev_name(&cxled->cxld.dev), res, cxled->dpa_res);
	if (res) {
		/* Trim region resource overlap with LMH */
		res->end = cxlrd->res->end;
	}
	/* Match endpoint decoder's DPA resource to root decoder's */
	cxled->dpa_res->end =
		cxled->dpa_res->start +
		resource_size(cxlrd->res) / cxled->cxld.interleave_ways - 1;
	dev_info(cxled_to_memdev(cxled)->dev.parent,
		 "Resources have been adjusted for LMH (%s: %pr, %pr)\n",
		 dev_name(&cxled->cxld.dev), res, cxled->dpa_res);
}
