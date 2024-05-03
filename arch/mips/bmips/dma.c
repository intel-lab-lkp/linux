// SPDX-License-Identifier: GPL-2.0+

#include <linux/types.h>
#include <linux/dma-map-ops.h>
#include <asm/bmips.h>
#include <asm/io.h>

bool bmips_rac_flush_disable;

void arch_sync_dma_for_cpu_all(void)
{
	u32 cfg;

	if (unlikely(bmips_rac_flush_disable))
		return;

	/* Flush stale data out of the readahead cache */
	cfg = __raw_readl(bmips_cbr_addr + BMIPS_RAC_CONFIG);
	__raw_writel(cfg | 0x100, bmips_cbr_addr + BMIPS_RAC_CONFIG);
	__raw_readl(bmips_cbr_addr + BMIPS_RAC_CONFIG);
}
