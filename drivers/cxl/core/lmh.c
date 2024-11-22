// SPDX-License-Identifier: GPL-2.0-only

#include <linux/range.h>
#include "cxl.h"

/* In x86 with memory hole, misaligned CFMWS range starts at 0x0 */
#define MISALIGNED_CFMWS_RANGE_BASE 0x0

/*
 * Match CXL Root and Endpoint Decoders by comparing SPA and HPA ranges.
 *
 * On x86, CFMWS ranges never intersect memory holes while endpoint decoders
 * HPA range sizes are always guaranteed aligned to NIW * 256MB; therefore,
 * the given endpoint decoder HPA range size is always expected aligned and
 * also larger than that of the matching root decoder
 */
bool arch_match_spa(struct cxl_root_decoder *cxlrd,
		    struct cxl_endpoint_decoder *cxled)
{
	struct range *r1, *r2;
	int niw;

	r1 = &cxlrd->cxlsd.cxld.hpa_range;
	r2 = &cxled->cxld.hpa_range;
	niw = cxled->cxld.interleave_ways;

	if (r1->start == MISALIGNED_CFMWS_RANGE_BASE &&
	    r1->start == r2->start && r1->end < r2->end &&
	    IS_ALIGNED(range_len(r2), niw * SZ_256M))
		return true;
	return false;
}

/* Similar to arch_match_spa(), it matches regions and decoders */
bool arch_match_region(struct cxl_region_params *p,
		       struct cxl_decoder *cxld)
{
	struct range *r = &cxld->hpa_range;
	struct resource *res = p->res;
	int niw = cxld->interleave_ways;

	if (res->start == MISALIGNED_CFMWS_RANGE_BASE &&
	    res->start == r->start && res->end < r->end &&
	    IS_ALIGNED(range_len(r), niw * SZ_256M))
		return true;
	return false;
}

void arch_trim_hpa_by_spa(struct resource *res,
			  struct cxl_root_decoder *cxlrd)
{
	res->end = cxlrd->res->end;
}
