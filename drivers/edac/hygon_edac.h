/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _HYGON_EDAC_H
#define _HYGON_EDAC_H

#include <linux/types.h>
#include <linux/processor.h>

struct amd64_pvt;

/*
 * TODO: Will move to <asm/hygon/node.h> in Hygon Node RFC next version.
 */
static inline bool is_hygon_f18h(void)
{
	return boot_cpu_data.x86_vendor == X86_VENDOR_HYGON &&
	       boot_cpu_data.x86 == 0x18;
}

u32 hygon_get_umc_base(struct amd64_pvt *pvt, u8 channel);
u8 hygon_get_umc_channel(u64 ipid);
bool hygon_supports_ddr5(void);
void hygon_per_family_init(struct amd64_pvt *pvt);

#endif /* _HYGON_EDAC_H */
