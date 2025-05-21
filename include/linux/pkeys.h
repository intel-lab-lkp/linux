/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PKEYS_H
#define _LINUX_PKEYS_H

#include <linux/mm.h>

#define ARCH_DEFAULT_PKEY	0

#ifdef CONFIG_ARCH_HAS_PKEYS
#include <asm/pkeys.h>
#else /* ! CONFIG_ARCH_HAS_PKEYS */
#define arch_max_pkey() (1)
#define execute_only_pkey(mm) (0)
#define arch_override_mprotect_pkey(vma, prot, pkey) (0)
#define PKEY_DEDICATED_EXECUTE_ONLY 0
#define ARCH_VM_PKEY_FLAGS 0

static inline int vma_pkey(struct vm_area_struct *vma)
{
	return 0;
}

static inline bool mm_pkey_is_allocated(struct mm_struct *mm, int pkey)
{
	return (pkey == 0);
}

static inline int mm_pkey_alloc(struct mm_struct *mm)
{
	return -1;
}

static inline int mm_pkey_free(struct mm_struct *mm, int pkey)
{
	return -EINVAL;
}

static inline int arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
			unsigned long init_val)
{
	return 0;
}

static inline bool arch_pkeys_enabled(void)
{
	return false;
}

#endif /* ! CONFIG_ARCH_HAS_PKEYS */

#ifndef CONFIG_ARCH_HAS_PERMISSIVE_PKEY

/*
 * Common name for value of the register that controls access to PKEYs
 * (called differently on different arches: PKRU, POR, AMR).
 */
typedef char pkey_reg_t;

/*
 * Sets PKEY access register to the most permissive value that allows
 * accesses to all PKEYs. Returns the current value of PKEY register.
 * Code should generally arrange switching back to the old value
 * using write_pkey_val(old_value).
 */
static inline pkey_reg_t write_permissive_pkey_val(void)
{
	return 0;
}

/*
 * Sets PKEY access register to a value that allows access to the 0 (default)
 * PKEY. Returns the current value of PKEY register.
 */
static inline pkey_reg_t enable_zero_pkey_val(void)
{
	return 0;
}

static inline void write_pkey_val(pkey_reg_t val) {}
#endif /* ! CONFIG_ARCH_HAS_PERMISSIVE_PKEY */

#endif /* _LINUX_PKEYS_H */
