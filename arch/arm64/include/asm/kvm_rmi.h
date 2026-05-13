/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#ifndef __ASM_KVM_RMI_H
#define __ASM_KVM_RMI_H

/**
 * struct realm - Additional per VM data for a Realm
 */
struct realm {
};

void kvm_init_rmi(void);

#endif /* __ASM_KVM_RMI_H */
