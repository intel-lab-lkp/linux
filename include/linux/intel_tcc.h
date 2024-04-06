/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  header for Intel TCC (thermal control circuitry) library
 *
 *  Copyright (C) 2022  Intel Corporation.
 */

#ifndef __INTEL_TCC_H__
#define __INTEL_TCC_H__

#include <linux/types.h>

int intel_tcc_get_tjmax(int cpu);
int intel_tcc_get_offset(int cpu);
int intel_tcc_set_offset(int cpu, int offset);
int intel_tcc_get_temp(int cpu, int *temp, bool pkg);
#ifdef CONFIG_INTEL_TCC
u32 get_tcc_offset_mask(void);
u32 intel_tcc_get_temp_mask(bool pkg);
#else
static inline u32 get_tcc_offset_mask(void) { return 0; }
/* Use the architectural bitmask of the temperature readout. No model checks. */
static inline u32 intel_tcc_get_temp_mask(bool pkg) { return 0x7f; }
#endif

#endif /* __INTEL_TCC_H__ */
