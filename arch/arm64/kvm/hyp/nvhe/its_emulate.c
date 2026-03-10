// SPDX-License-Identifier: GPL-2.0-only

#include <asm/kvm_pkvm.h>
#include <nvhe/mem_protect.h>


void pkvm_handle_forward_req(struct pkvm_protected_reg *region, u64 offset, bool write,
			     u64 *reg, u8 reg_size)
{
	void __iomem *addr = __hyp_va((region->start_pfn << PAGE_SHIFT) + offset);

	if (reg_size == sizeof(u32)) {
		if (!write)
			*reg = readl_relaxed(addr);
		else
			writel_relaxed(*reg, addr);
	} else if (reg_size == sizeof(u64)) {
		if (!write)
			*reg = readq_relaxed(addr);
		else
			writeq_relaxed(*reg, addr);
	}
}
