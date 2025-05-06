/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_TABLE_API_H
#define _ASM_X86_CPUID_TABLE_API_H

#include <asm/processor.h>

void cpuid_scan_cpu(struct cpuinfo_x86 *c);

#endif /* _ASM_X86_CPUID_TABLE_API_H */
