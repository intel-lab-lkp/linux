/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CXL_PRIVATE_REGION_H__
#define __CXL_PRIVATE_REGION_H__

struct cxl_region;

int cxl_register_private_region(struct cxl_region *cxlr);
void cxl_unregister_private_region(struct cxl_region *cxlr);

/* Type-specific registration functions - called from region.c dispatch */
int cxl_register_zswap_region(struct cxl_region *cxlr);
void cxl_unregister_zswap_region(struct cxl_region *cxlr);

#endif /* __CXL_PRIVATE_REGION_H__ */
