// SPDX-License-Identifier: GPL-2.0

#include <linux/srcu.h>

__rust_helper int rust_helper_init_srcu_struct_with_key(struct srcu_struct *ssp,
							const char *name,
							struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	return __init_srcu_struct(ssp, name, key);
#else /* !CONFIG_DEBUG_LOCK_ALLOC */
	return init_srcu_struct(ssp);
#endif /* CONFIG_DEBUG_LOCK_ALLOC */
}

__rust_helper int rust_helper_srcu_read_lock(struct srcu_struct *ssp)
{
	return srcu_read_lock(ssp);
}

__rust_helper void rust_helper_srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	srcu_read_unlock(ssp, idx);
}
