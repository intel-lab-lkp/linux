// SPDX-License-Identifier: GPL-2.0
/*
 * Secure Launch early measurement and validation routines.
 *
 * Copyright (c) 2026, Oracle and/or its affiliates.
 * Copyright (c) 2026 Apertus Solutions, LLC
 */

#include <asm/msr.h>
#include <asm/mtrr.h>
#include <asm/processor-flags.h>
#include <asm/asm-offsets.h>
#include <asm/bootparam.h>
#include <asm/shared/msr.h>
#include <linux/efi.h>
#include <linux/slr_table.h>
#include <linux/slaunch.h>

#include "tpm.h"

u32 sl_cpu_type __initdata;
u32 sl_mle_start __initdata;

void sl_main(void *bootparams);

asmlinkage __visible __init void sl_main(void *bootparams)
{
}
