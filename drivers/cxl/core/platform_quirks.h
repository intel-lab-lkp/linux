/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */

#include "cxl.h"

#ifdef CONFIG_CXL_PLATFORM_QUIRKS
bool platform_cxlrd_matches_cxled(const struct cxl_root_decoder *cxlrd,
				  const struct cxl_endpoint_decoder *cxled);
bool platform_region_matches_cxld(const struct cxl_region_params *p,
				  const struct cxl_decoder *cxld);
void platform_adjust_resources(struct resource *res,
			       struct cxl_endpoint_decoder *cxled,
			       const struct cxl_root_decoder *cxlrd,
			       const struct device *region_dev);
#else
static inline bool
platform_root_decoder_contains(const struct cxl_root_decoder *cxlrd,
			       const struct cxl_endpoint_decoder *cxled)
{
	return false;
}

static inline bool
platform_region_matches_cxld(const struct cxl_region_params *p,
			     const struct cxl_decoder *cxld)
{
	return false;
}

static inline void
platform_adjust_resources(struct resource *res,
			  struct cxl_endpoint_decoder *cxled,
			  const struct cxl_root_decoder *cxlrd,
			  const struct device *region_dev)
{ }
#endif /* CONFIG_CXL_PLATFORM_QUIRKS */
