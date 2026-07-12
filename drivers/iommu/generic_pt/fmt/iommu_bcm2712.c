// SPDX-License-Identifier: GPL-2.0-only
#define PT_FMT bcm2712
#define PT_SUPPORTED_FEATURES BIT(PT_FEAT_DMA_INCOHERENT)
#define PT_FORCE_ENABLED_FEATURES 0

/* BCM2712 has no spare software bits for tracking cache flushes */
#define PT_SW_BIT_NOT_PRESENT

#include <linux/generic_pt/iommu.h>
#include "iommu_template.h"
