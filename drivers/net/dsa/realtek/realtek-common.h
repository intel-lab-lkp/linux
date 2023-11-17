/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _REALTEK_INTERFACE_H
#define _REALTEK_INTERFACE_H

#include <linux/regmap.h>

struct realtek_variant_entry {
	const struct realtek_variant *variant;
	const char *compatible;
	struct module *owner;
	struct list_head list;
};

#define module_realtek_variant(__variant, __compatible)			\
static struct realtek_variant_entry __variant ## _entry = {		\
	.compatible = __compatible,					\
	.variant = &(__variant),					\
	.owner = THIS_MODULE,						\
};									\
static int __init realtek_variant_module_init(void)			\
{									\
	realtek_variant_register(&__variant ## _entry);			\
	return 0;							\
}									\
module_init(realtek_variant_module_init)				\
									\
static void __exit realtek_variant_module_exit(void)			\
{									\
	realtek_variant_unregister(&__variant ## _entry);		\
}									\
module_exit(realtek_variant_module_exit);				\
									\
MODULE_ALIAS(__compatible)

void realtek_variant_register(struct realtek_variant_entry *variant_entry);
void realtek_variant_unregister(struct realtek_variant_entry *variant_entry);

extern const struct of_device_id realtek_common_of_match[];

void realtek_common_lock(void *ctx);
void realtek_common_unlock(void *ctx);
struct realtek_priv *realtek_common_probe(struct device *dev,
		struct regmap_config rc, struct regmap_config rc_nolock);
void realtek_common_remove(struct realtek_priv *priv);
const struct realtek_variant *realtek_variant_get(const char *compatible);
void realtek_variant_put(const struct realtek_variant *var);

#endif
