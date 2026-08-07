// SPDX-License-Identifier: GPL-2.0-only

#include <asm/kvm_pkvm.h>
#include <nvhe/mem_protect.h>

void its_emulate_forward_req(struct pkvm_protected_reg *region, u64 offset, bool write, u64 *reg,
			     u8 reg_size)
{
	void __iomem *addr = __hyp_va(PFN_PHYS(region->pfn) + offset);

	switch (reg_size) {
	case 1:
		if (!write)
			*reg = readb_relaxed(addr);
		else
			writeb_relaxed(*reg, addr);
		break;
	case 2:
		if (!write)
			*reg = readw_relaxed(addr);
		else
			writew_relaxed(*reg, addr);
		break;
	case 4:
		if (!write)
			*reg = readl_relaxed(addr);
		else
			writel_relaxed(*reg, addr);
		break;
	case 8:
		if (!write)
			*reg = readq_relaxed(addr);
		else
			writeq_relaxed(*reg, addr);
		break;
	}
}
