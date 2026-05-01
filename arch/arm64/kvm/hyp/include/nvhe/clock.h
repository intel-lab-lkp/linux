/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_HYP_NVHE_CLOCK_H
#define __ARM64_KVM_HYP_NVHE_CLOCK_H
#include <linux/types.h>

#include <asm/kvm_hyp.h>

void hyp_clock_update(u32 mult, u32 shift, u64 epoch_ns, u64 epoch_cyc);
u64 hyp_clock_ns(void);
int hyp_clock_init(void);
#endif
