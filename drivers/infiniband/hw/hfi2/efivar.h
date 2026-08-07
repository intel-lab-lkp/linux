/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015, 2016 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_EFIVAR_H
#define _HFI2_EFIVAR_H

#include <linux/efi.h>

#include "hfi2.h"

int hfi2_read_hfi2_efi_var(struct hfi2_devdata *dd, const char *kind,
		      unsigned long *size, void **return_data);

#endif /* _HFI2_EFIVAR_H */
