/* SPDX-License-Identifier: GPL-2.0-only */

#include "cxl.h"

#ifdef CONFIG_CXL_ARCH_LOW_MEMORY_HOLE
bool arch_match_spa(struct cxl_root_decoder *cxlrd,
		    struct cxl_endpoint_decoder *cxled);
bool arch_match_region(struct cxl_region_params *p, struct cxl_decoder *cxld);
void arch_adjust_region_resource(struct resource *res,
				 struct cxl_root_decoder *cxlrd);
#else
static bool arch_match_spa(struct cxl_root_decoder *cxlrd,
			   struct cxl_endpoint_decoder *cxled)
{
	return false;
}

static bool arch_match_region(struct cxl_region_params *p,
			      struct cxl_decoder *cxld)
{
	return false;
}

static void arch_adjust_region_resource(struct resource *res,
					struct cxl_root_decoder *cxlrd)
{
}
#endif /* CXL_ARCH_LOW_MEMORY_HOLE */
