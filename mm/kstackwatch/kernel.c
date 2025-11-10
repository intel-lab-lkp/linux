// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kstackwatch.h>
#include <linux/kstrtox.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/string.h>

static struct ksw_config *ksw_config;

struct param_map {
	const char *name;       /* long name */
	const char *short_name; /* short name (2 letters) */
	size_t offset;          /* offsetof(struct ksw_config, field) */
	bool is_string;         /* true for string */
};

/* macro generates both long and short name automatically */
#define PMAP(field, short, is_str) \
	{ #field, #short, offsetof(struct ksw_config, field), is_str }

static const struct param_map ksw_params[] = {
	PMAP(func_name,   fn, true),
	PMAP(func_offset, fo, false),
	PMAP(depth,       dp, false),
	PMAP(max_watch,   mw, false),
	PMAP(sp_offset,   so, false),
	PMAP(watch_len,   wl, false),
	PMAP(auto_canary, ac, false),
	PMAP(panic_hit,   ph, false),
};

static int ksw_parse_param(struct ksw_config *config, const char *key,
			   const char *val)
{
	const struct param_map *pm = NULL;
	int ret;

	for (int i = 0; i < ARRAY_SIZE(ksw_params); i++) {
		if (strcmp(key, ksw_params[i].name) == 0 ||
		    strcmp(key, ksw_params[i].short_name) == 0) {
			pm = &ksw_params[i];
			break;
		}
	}

	if (!pm)
		return -EINVAL;

	if (pm->is_string) {
		char **dst = (char **)((char *)config + pm->offset);
		*dst = kstrdup(val, GFP_KERNEL);
		if (!*dst)
			return -ENOMEM;
	} else {
		ret = kstrtou16(val, 0, (u16 *)((char *)config + pm->offset));
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Configuration string format:
 *    param_name=<value> [param_name=<value> ...]
 *
 * Required parameters:
 * - func_name  |fn (str) : target function name
 * - func_offset|fo (u16) : instruction pointer offset
 *
 * Optional parameters:
 * - depth      |dp (u16) : recursion depth
 * - max_watch  |mw (u16) : maximum number of watchpoints
 * - sp_offset  |so (u16) : offset from stack pointer at func_offset
 * - watch_len  |wl (u16) : watch length (1,2,4,8)
 */
static int __maybe_unused ksw_parse_config(char *buf, struct ksw_config *config)
{
	char *part, *key, *val;
	int ret;

	kfree(config->func_name);
	kfree(config->user_input);
	memset(ksw_config, 0, sizeof(*ksw_config));

	buf = strim(buf);
	config->user_input = kstrdup(buf, GFP_KERNEL);
	if (!config->user_input)
		return -ENOMEM;

	while ((part = strsep(&buf, " \t\n")) != NULL) {
		if (*part == '\0')
			continue;

		key = strsep(&part, "=");
		val = part;
		if (!key || !val)
			continue;
		ret = ksw_parse_param(config, key, val);
		if (ret)
			pr_warn("unsupported param %s=%s", key, val);
	}

	if (!config->func_name) {
		pr_err("Missing required parameters: function or func_offset\n");
		return -EINVAL;
	}

	return 0;
}

static int __init kstackwatch_init(void)
{
	ksw_config = kzalloc(sizeof(*ksw_config), GFP_KERNEL);
	if (!ksw_config)
		return -ENOMEM;

	pr_info("module loaded\n");
	return 0;
}

static void __exit kstackwatch_exit(void)
{
	kfree(ksw_config);

	pr_info("module unloaded\n");
}

module_init(kstackwatch_init);
module_exit(kstackwatch_exit);

MODULE_AUTHOR("Jinchao Wang");
MODULE_DESCRIPTION("Kernel Stack Watch");
MODULE_LICENSE("GPL");

