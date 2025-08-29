/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __CXL_TEST_H__
#define __CXL_TEST_H__

#include <linux/acpi.h>

/* Function declarations only visible to test code */

/* XOR calculation function from drivers/cxl/acpi.c */
u64 cxl_do_xormap_calc(struct cxl_cxims_data *cximsd, u64 addr, int hbiw);

/* Address translation functions from drivers/cxl/core/region.c */
u64 cxl_calculate_hpa_offset(u64 dpa_offset, int pos, u8 eiw, u16 eig);
u64 cxl_calculate_dpa_offset(u64 hpa_offset, u8 eiw, u16 eig);
int cxl_calculate_position(u64 hpa_offset, u8 eiw, u16 eig);

#endif
