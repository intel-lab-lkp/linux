/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __NVHE_ITS_EMULATE_H
#define __NVHE_ITS_EMULATE_H

#include <asm/kvm_pkvm.h>

struct its_host_state;

int pkvm_its_emulate_setup(phys_addr_t dev_addr, struct its_host_state *host_state, void *priv,
			   size_t priv_num_pages);
void pkvm_its_emulate_handler(struct pkvm_protected_reg *region, u64 offset, bool write, u64 *reg,
			      u8 reg_size);
#endif /* __NVHE_ITS_EMULATE_H */
