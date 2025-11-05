// SPDX-License-Identifier: GPL-2.0
/*
 * Warn about uninitialized automatic variables that use the
 * __attribute__((cleanup(...))) attribute.
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

static bool has_declaration_initializer(tree var)
{
	if (DECL_INITIAL(var))
		return true;

#ifdef DECL_INITIALIZED_P
	if (DECL_INITIALIZED_P(var))
		return true;
#endif

	return false;
}

static void warn_if_uninitialized(tree var)
{
	location_t loc;
	bool saved_warning_as_error;

	if (has_declaration_initializer(var))
		return;

	loc = DECL_SOURCE_LOCATION(var);
	if (loc == UNKNOWN_LOCATION)
		return;

	/* Temporarily disable treating warnings as errors for this specific warning */
	saved_warning_as_error = global_dc->warning_as_error_requested_p();
	global_dc->set_warning_as_error_requested(false);
	warning_at(
		loc, 0,
		"%qD declared with cleanup attribute is not initialized at declaration",
		var);
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
