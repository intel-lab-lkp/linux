// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 by Li Chen <me@linux.beauty>
 *
 * This gcc plugin warns about problematic patterns when using variables
 * with __attribute__((cleanup(...))). The cleanup attribute helpers
 * (__free, DEFINE_FREE, etc.) are designed to automatically clean up
 * resources when variables go out of scope, following LIFO ordering.
 * However, certain patterns can lead to interdependency issues.
 *
 * The plugin detects two problematic patterns:
 *
 * 1. Uninitialized cleanup variables:
 *    Variables declared with cleanup attributes but not initialized can
 *    cause issues when cleanup functions are called on undefined values.
 *
 *    Example:
 *    void func(void)
 *    {
 *        struct resource *res __free(cleanup);  // Warning: not initialized
 *        res = acquire_resource();
 *        // ...
 *    }
 *
 *    Should be:
 *    void func(void)
 *    {
 *        struct resource *res __free(cleanup) = acquire_resource();
 *        // ...
 *    }
 *
 * 2. NULL-initialized cleanup variables:
 *    The "__free(...) = NULL" pattern at function top can cause
 *    interdependency problems, especially when combined with guards or
 *    multiple cleanup variables, as documented in include/linux/cleanup.h.
 *
 *    Example:
 *    void func(void)
 *    {
 *        struct resource *res __free(cleanup) = NULL;  // Warning: NULL init
 *        guard(mutex)(&lock);
 *        res = acquire_resource();
 *        // cleanup may run without lock held!
 *    }
 *
 *    Should be:
 *    void func(void)
 *    {
 *        guard(mutex)(&lock);
 *        struct resource *res __free(cleanup) = acquire_resource();
 *        // ...
 *    }
 *
 * The plugin provides clear warnings to help developers identify these
 * patterns during compilation. Importantly, these warnings are not
 * converted to errors by -Werror, allowing builds to continue while
 * still alerting developers to potential issues.
 *
 * Options:
 * - None currently supported
 *
 * Attribute: __attribute__((cleanup(...)))
 *  The cleanup gcc attribute can be used on automatic variables to
 *  specify a function to be called when the variable goes out of scope.
 *  This plugin validates that such variables are properly initialized
 *  at declaration time to avoid interdependency issues.
 */

#include "gcc-common.h"

__visible int plugin_is_GPL_compatible;

static struct plugin_info cleanup_plugin_info = {
	.version = PLUGIN_VERSION,
	.help = "Warn when cleanup attribute variables lack initializers\n",
};

static bool has_cleanup_attribute(tree var)
{
	tree attrs;

	attrs = DECL_ATTRIBUTES(var);
	if (!attrs)
		return false;

	return lookup_attribute("cleanup", attrs) != NULL_TREE;
}

static bool is_candidate_decl(tree var)
{
	if (TREE_CODE(var) != VAR_DECL)
		return false;

	if (DECL_ARTIFICIAL(var))
		return false;

	if (TREE_STATIC(var) || DECL_EXTERNAL(var))
		return false;

	if (!has_cleanup_attribute(var))
		return false;

	return true;
}

static bool is_null_initializer(tree initial)
{
	if (!initial)
		return false;

	/* Check if the initializer is NULL pointer constant */
	if (initial == null_pointer_node)
		return true;

	/* Check if it's an integer constant zero (which can be NULL) */
	if (TREE_CODE(initial) == INTEGER_CST && integer_zerop(initial))
		return true;

	return false;
}

static bool has_valid_declaration_initializer(tree var)
{
	tree initial = DECL_INITIAL(var);

	/* No initializer at all */
	if (!initial) {
#ifdef DECL_INITIALIZED_P
		if (DECL_INITIALIZED_P(var))
			return true;
#endif
		return false;
	}

	/* NULL initialization is considered invalid for cleanup variables */
	if (is_null_initializer(initial))
		return false;

	/* Any other non-NULL initializer is valid */
	return true;
}

static void warn_if_uninitialized(tree var)
{
	location_t loc;
	bool saved_warning_as_error;
	tree initial = DECL_INITIAL(var);
	bool is_null_init = false;

	if (has_valid_declaration_initializer(var))
		return;

	loc = DECL_SOURCE_LOCATION(var);
	if (loc == UNKNOWN_LOCATION)
		return;

	/* Check if it's a NULL initialization */
	is_null_init = initial && is_null_initializer(initial);

	/* Temporarily disable treating warnings as errors for this specific warning */
	saved_warning_as_error = global_dc->warning_as_error_requested_p();
	global_dc->set_warning_as_error_requested(false);
	if (is_null_init) {
		warning_at(
			loc, 0,
			"%qD declared with cleanup attribute is initialized to NULL at declaration",
			var);
	} else {
		warning_at(
			loc, 0,
			"%qD declared with cleanup attribute is not initialized at declaration",
			var);
	}
	/* Restore the original setting */
	global_dc->set_warning_as_error_requested(saved_warning_as_error);
}

static void cleanup_finish_decl(void *gcc_data, void *user_data)
{
	tree var = (tree)gcc_data;

	(void)user_data;

	if (!is_candidate_decl(var))
		return;

	warn_if_uninitialized(var);
}

__visible int plugin_init(struct plugin_name_args *plugin_info,
			  struct plugin_gcc_version *version)
{
	if (!plugin_default_version_check(version, &gcc_version)) {
		error(G_("incompatible gcc/plugin versions"));
		return 1;
	}

	register_callback(plugin_info->base_name, PLUGIN_INFO, NULL,
			  &cleanup_plugin_info);
	register_callback(plugin_info->base_name, PLUGIN_FINISH_DECL,
			  cleanup_finish_decl, NULL);

	return 0;
}
