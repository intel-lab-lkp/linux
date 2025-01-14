// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include "cxl.h"
#include "lmh.h"

/* Exporting of cxl_core symbols that are only used by cxl_test */
EXPORT_SYMBOL_NS_GPL(cxl_num_decoders_committed, "CXL");
EXPORT_SYMBOL_NS_GPL(set_mock_cfmws0_range_start, "CXL");
