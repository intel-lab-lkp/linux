// SPDX-License-Identifier: GPL-2.0

#include <linux/lsm_hooks.h>

/*
 * Hook list initializers.
 *
 * struct security_hook_list carries __randomize_layout, so its fields must
 * never be set by Rust code directly (bindgen sees only the "natural" field
 * order, which may differ when CONFIG_RANDSTRUCT is enabled).  These helpers
 * wrap LSM_HOOK_INIT(), the C macro the kernel already uses for this purpose,
 * so that Rust can safely obtain an initialised security_hook_list without
 * ever touching the struct's fields directly.
 */

__rust_helper void
rust_helper_lsm_hook_init_file_open(struct security_hook_list *hl,
				    int (*fn)(struct file *))
{
	*hl = (struct security_hook_list)LSM_HOOK_INIT(file_open, fn);
}

__rust_helper void
rust_helper_lsm_hook_init_task_alloc(struct security_hook_list *hl,
				     int (*fn)(struct task_struct *, u64))
{
	*hl = (struct security_hook_list)LSM_HOOK_INIT(task_alloc, fn);
}

__rust_helper void
rust_helper_lsm_hook_init_task_free(struct security_hook_list *hl,
				    void (*fn)(struct task_struct *))
{
	*hl = (struct security_hook_list)LSM_HOOK_INIT(task_free, fn);
}

/*
 * security_add_hooks() is __init — it installs callbacks into the static-call
 * table and must only be called during kernel initialisation.  This wrapper
 * keeps the __init annotation so the linker places it alongside the rest of
 * the init text, and Rust callers invoke it only from their own __init path
 * (the lsm_info.init callback).
 */
void __init
rust_helper_security_add_hooks(struct security_hook_list *hooks, int count,
			       const struct lsm_id *lsmid)
{
	security_add_hooks(hooks, count, lsmid);
}
