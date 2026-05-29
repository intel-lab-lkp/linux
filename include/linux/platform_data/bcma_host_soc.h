/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_PLATFORM_DATA_BCMA_HOST_SOC_H
#define _LINUX_PLATFORM_DATA_BCMA_HOST_SOC_H

#include <linux/types.h>

/**
 * struct bcma_host_soc_pdata - SoC-specific configuration for bcma-host-soc.
 *
 * Used by parent bridge drivers that instantiate bcma-host-soc as a child
 * platform_device (e.g. the BCM6362 WLAN SHIM bridge). The legacy
 * brcm,bus-axi DT path uses default values and does not supply this.
 *
 * @big_endian:    Backplane registers are big-endian peripherals on a
 *                 big-endian CPU. Selects ioread/iowrite *be helpers for
 *                 all bcma register accesses on this bus.
 * @shim_attached: Cores on this backplane do not publish per-core DMP
 *                 wrappers (NMW=NSW=0 in the EROM); clock and reset
 *                 gating instead lives in a SoC-level "SHIM" Control
 *                 register peephole reached through @shim_iomem.
 * @shim_iomem:    Pre-mapped iomem pointer for the SHIM peephole.
 *                 Lifetime is owned by the parent bridge driver; the
 *                 bcma-host-soc driver must not iounmap it.
 */
struct bcma_host_soc_pdata {
	bool		big_endian;
	bool		shim_attached;
	void __iomem	*shim_iomem;
};

#endif /* _LINUX_PLATFORM_DATA_BCMA_HOST_SOC_H */
