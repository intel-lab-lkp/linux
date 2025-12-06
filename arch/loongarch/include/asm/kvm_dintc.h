/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Loongson Technology Corporation Limited
 */

#ifndef __ASM_KVM_DINTC_H
#define __ASM_KVM_DINTC_H


struct loongarch_dintc  {
	struct kvm *kvm;
	uint64_t msg_addr_base;
	uint64_t msg_addr_size;
};

struct dintc_state {
	atomic64_t  vector_map[4];
};

int kvm_loongarch_register_dintc_device(void);
#endif
