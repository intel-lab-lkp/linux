/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_AMD_SMN_H
#define _ASM_X86_AMD_SMN_H

#include <linux/types.h>
#include <asm/amd_node.h>

int __must_check amd_smn_read(u16 node, u32 address, u32 *value);
int __must_check amd_smn_write(u16 node, u32 address, u32 value);

#endif /* _ASM_X86_AMD_SMN_H */
