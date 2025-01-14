// SPDX-License-Identifier: GPL-2.0-only

#include <linux/range.h>
#include "lmh.h"

/* Start of CFMWS range that end before x86 Low Memory Holes */
#define LMH_CFMWS_RANGE_START 0x0ULL

/*
 * Match CXL Root and Endpoint Decoders by comparing SPA and HPA ranges.
 *
 * On x86, CFMWS ranges never intersect memory holes while endpoint decoders
 * HPA range sizes are always guaranteed aligned to NIW * 256MB; therefore,
 * the given endpoint decoder HPA range size is always expected aligned and
 * also larger than that of the matching root decoder. If there are LMH's,
 * the root decoder range end is always less than SZ_4G.
 */
bool arch_match_spa(struct cxl_root_decoder *cxlrd,
		    struct cxl_endpoint_decoder *cxled)
{
	struct range *r1, *r2;
	int niw;

	r1 = &cxlrd->cxlsd.cxld.hpa_range;
	r2 = &cxled->cxld.hpa_range;
	niw = cxled->cxld.interleave_ways;

	if (r1->start == LMH_CFMWS_RANGE_START && r1->start == r2->start &&
	    r1->end < (LMH_CFMWS_RANGE_START + SZ_4G) && r1->end < r2->end &&
	    IS_ALIGNED(range_len(r2), niw * SZ_256M))
		return true;

	return false;
}

/* Similar to arch_match_spa(), it matches regions and decoders */
bool arch_match_region(struct cxl_region_params *p, struct cxl_decoder *cxld)
{
	struct range *r = &cxld->hpa_range;
	struct resource *res = p->res;
	int niw = cxld->interleave_ways;

	if (res->start == LMH_CFMWS_RANGE_START && res->start == r->start &&
	    res->end < (LMH_CFMWS_RANGE_START + SZ_4G) && res->end < r->end &&
	    IS_ALIGNED(range_len(r), niw * SZ_256M))
		return true;

	return false;
}

void arch_adjust_region_resource(struct resource *res,
				 struct cxl_root_decoder *cxlrd)
{
	res->end = cxlrd->res->end;
}
