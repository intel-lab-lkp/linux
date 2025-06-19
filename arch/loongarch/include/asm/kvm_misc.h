/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Loongson Technology Corporation Limited
 */

#ifndef __ASM_KVM_MISC_H
#define __ASM_KVM_MISC_H

#include <asm/loongarch.h>

#define MISC_BASE		LOONGARCH_IOCSR_MISC_FUNC
#define MISC_SIZE		0x8

int kvm_loongarch_create_misc(struct kvm *kvm);
void kvm_loongarch_destroy_misc(struct kvm *kvm);

#endif /* __ASM_KVM_MISC_H */
