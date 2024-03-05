/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_DMA_DW_AXI_H
#define __LINUX_DMA_DW_AXI_H

#include <linux/types.h>

struct dw_axi_peripheral_config {
#define DWAXIDMAC_STARFIVE_SM_ALGO	BIT(0)
	u32 quirks;
};
#endif /* __LINUX_DMA_DW_AXI_H */
