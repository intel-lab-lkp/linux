// SPDX-License-Identifier: GPL-2.0

/* Export the PF crate's symbols used by the VF driver. */

#include <linux/export.h>

#define EXPORT_SYMBOL_RUST_GPL(sym) extern int sym; EXPORT_SYMBOL_GPL(sym)

#include "exports_rust_driver_sriov_pf_generated.h"
