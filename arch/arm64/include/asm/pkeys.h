/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Arm Ltd.
 *
 * Based on arch/x86/include/asm/pkeys.h
*/

#ifndef _ASM_ARM64_PKEYS_H
#define _ASM_ARM64_PKEYS_H

#define ARCH_VM_PKEY_FLAGS (VM_PKEY_BIT0 | VM_PKEY_BIT1 | VM_PKEY_BIT2)

#define arch_max_pkey() 0

int arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val);

static inline bool arch_pkeys_enabled(void)
{
	return false;
}

static inline int vma_pkey(struct vm_area_struct *vma)
{
	return -1;
}

static inline int arch_override_mprotect_pkey(struct vm_area_struct *vma,
		int prot, int pkey)
{
	return -1;
}

static inline int execute_only_pkey(struct mm_struct *mm)
{
	return -1;
}

static inline bool mm_pkey_is_allocated(struct mm_struct *mm, int pkey)
{
	return false;
}

static inline int mm_pkey_alloc(struct mm_struct *mm)
{
	return -1;
}

static inline int mm_pkey_free(struct mm_struct *mm, int pkey)
{
	return -EINVAL;
}

#endif /* _ASM_ARM64_PKEYS_H */
