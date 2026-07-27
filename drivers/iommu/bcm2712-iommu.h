/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BCM2712_IOMMU_H
#define _BCM2712_IOMMU_H

#include <linux/sizes.h>

/*
 * Define an aperture inside which IOMMU mappings are operated.
 *
 * This aperture must be placed within a high, unused part of the address space:
 * the IOMMU hardware caps the maximum reachable address at the top of the
 * aperture, and we ensure that regular RAM/MMIO is available to non-IOMMU-users
 * by creating a bypass window for all addresses below the aperture start.
 *
 * The aperture is hardcoded to 40GiB here, which is safely above RAM/MMIO
 * on the BCM2712 SoC.
 */
#define BCM2712_APERTURE_BASE    (40ULL << 30)
#define BCM2712_APERTURE_SIZE    SZ_4G
#define BCM2712_APERTURE_END     (BCM2712_APERTURE_BASE + BCM2712_APERTURE_SIZE)

#endif
