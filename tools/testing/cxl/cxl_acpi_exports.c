// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include "cxl_test.h"

/* Export of cxl_acpi (acpi.o) symbol used only by cxl_translate */
EXPORT_SYMBOL_NS_GPL(cxl_do_xormap_calc, "CXL");
