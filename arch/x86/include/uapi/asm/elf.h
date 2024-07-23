/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_X86_ELF_H
#define _UAPI_ASM_X86_ELF_H

struct x86_xfeat_component {
	u32 type;
	u32 size;
	u32 offset;
	u32 flags;
} __packed;

_Static_assert(sizeof(struct x86_xfeat_component)%4 == 0, "x86_xfeat_component is not aligned");

#endif /* _UAPI_ASM_X86_ELF_H */

