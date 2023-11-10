/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIVEPATCH_CORE_H
#define _LIVEPATCH_CORE_H

#include <linux/livepatch.h>

extern struct mutex klp_mutex;
extern struct list_head klp_patches;

#define klp_for_each_patch_safe(patch, tmp_patch)		\
	list_for_each_entry_safe(patch, tmp_patch, &klp_patches, list)

#define klp_for_each_patch(patch)	\
	list_for_each_entry(patch, &klp_patches, list)

void klp_free_patch_async(struct klp_patch *patch);
void klp_free_replaced_patches_async(struct klp_patch *new_patch);
void klp_unpatch_replaced_patches(struct klp_patch *new_patch);
void klp_discard_nops(struct klp_patch *new_patch);

static inline bool klp_is_object_loaded(struct klp_object *obj)
{
	return !obj->name || obj->mod;
}

#endif /* _LIVEPATCH_CORE_H */
