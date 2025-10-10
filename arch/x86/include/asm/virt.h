/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_VIRT_H
#define _ASM_X86_VIRT_H

#include <asm/reboot.h>

#if IS_ENABLED(CONFIG_KVM_X86)
extern bool virt_rebooting;

void __init x86_virt_init(void);

int x86_virt_get_cpu(int feat);
void x86_virt_put_cpu(int feat);

void x86_virt_register_emergency_callback(cpu_emergency_virt_cb *callback);
void x86_virt_unregister_emergency_callback(cpu_emergency_virt_cb *callback);
#else
static __always_inline void x86_virt_init(void) {}
#endif

#endif /* _ASM_X86_VIRT_H */
