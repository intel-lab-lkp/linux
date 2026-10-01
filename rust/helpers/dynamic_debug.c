// SPDX-License-Identifier: GPL-2.0

#include <linux/dynamic_debug.h>
#include <linux/printk.h>

#ifdef CONFIG_DYNAMIC_DEBUG_CORE
#ifndef CONFIG_JUMP_LABEL
__rust_helper bool rust_helper_ddebug_enabled(struct _ddebug *descriptor)
{
	/* Match the C dynamic debug branch without creating a Rust reference. */
	return descriptor->flags & _DPRINTK_FLAGS_PRINT;
}
#endif

/* The printing helpers receive Rust fmt::Arguments for the %pA bridge. */
__rust_helper void rust_helper_ddebug_print(struct _ddebug *descriptor,
					    const char *prefix,
					    const void *args)
{
	__dynamic_pr_debug(descriptor, "%s: %pA", prefix, args);
	if (descriptor->flags & _DPRINTK_FLAGS_INCL_STACK)
		dump_stack();
}

#endif
