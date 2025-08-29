// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include "cxl.h"
#include "cxl_test.h"

/* Exporting of cxl_core symbols that are only used by cxl_test */
EXPORT_SYMBOL_NS_GPL(cxl_num_decoders_committed, "CXL");

/*
 * Exporting of cxl_core symbols used only by the cxl_translate module.
 *
 * Note: checkpatch warns about EXPORT_SYMBOL placement, but this is
 * the established pattern for CXL test exports where functions are
 * defined in drivers/cxl/core/.
 */
EXPORT_SYMBOL_NS_GPL(cxl_calculate_hpa_offset, "CXL");
EXPORT_SYMBOL_NS_GPL(cxl_calculate_dpa_offset, "CXL");
EXPORT_SYMBOL_NS_GPL(cxl_calculate_position, "CXL");
