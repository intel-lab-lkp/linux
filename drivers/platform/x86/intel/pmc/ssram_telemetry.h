/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Intel PMC SSRAM TELEMETRY PCI Driver Header File
 *
 * Copyright (c) 2024, Intel Corporation.
 * All Rights Reserved.
 *
 */

#ifndef PMC_SSRAM_H
#define PMC_SSRAM_H

/**
 * struct pmc_ssram_telemetry - Structure to keep pmc info in ssram device
 * @devid:		device id of the pmc device
 * @base_addr:		contains PWRM base address
 */
struct pmc_ssram_telemetry {
	u16 devid;
	u64 base_addr;
};

#if IS_REACHABLE(CONFIG_INTEL_PMC_SSRAM_TELEMETRY)
/**
 * pmc_ssram_telemetry_get_pmc_info() - Get a PMC devid and base_addr information
 * @pmc_idx:               Index of the PMC
 * @pmc_ssram_telemetry:   pmc_ssram_telemetry structure to store the PMC information
 *
 * Return:
 * * 0           - Success
 * * -EAGAIN     - Probe function has not finished yet. Try again.
 * * -EINVAL     - Invalid pmc_idx
 * * -ENODEV     - PMC device is not available
 */
int pmc_ssram_telemetry_get_pmc_info(unsigned int pmc_idx,
				     struct pmc_ssram_telemetry *pmc_ssram_telemetry);
#else /* !CONFIG_INTEL_PMC_SSRAM_TELEMETRY */
static inline int pmc_ssram_telemetry_get_pmc_info(int pmc_idx,
						   struct pmc_ssram_telemetry *pmc_ssram_telemetry)
{
	return -ENODEV;
}
#endif /* CONFIG_INTEL_PMC_SSRAM_TELEMETRY */

#endif /* PMC_SSRAM_H */
