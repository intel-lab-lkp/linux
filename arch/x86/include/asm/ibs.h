/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IBS_H
#define _ASM_X86_IBS_H

void hw_access_profiling_start(void);
void hw_access_profiling_stop(void);
extern bool arch_hw_access_profiling;

#endif /* _ASM_X86_IBS_H */
