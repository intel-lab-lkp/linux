// SPDX-License-Identifier: GPL-2.0-only
#define PT_FMT bcm2712
#define PT_SUPPORTED_FEATURES (BIT(PT_FEAT_DMA_INCOHERENT) | BIT(PT_FEAT_NO_SW_BIT))
/* BCM2712 has no spare software bits for tracking cache flushes */
#define PT_FORCE_ENABLED_FEATURES BIT(PT_FEAT_NO_SW_BIT)

#include <linux/generic_pt/iommu.h>
#include "iommu_template.h"
