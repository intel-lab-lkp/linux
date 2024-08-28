/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIVEPATCH_PATCH_H
#define _LIVEPATCH_PATCH_H

#include <linux/livepatch.h>
#include <linux/list.h>
#include <linux/ftrace.h>

struct klp_ops *klp_find_ops(void *old_func);

int klp_patch_object(struct klp_object *obj);
void klp_unpatch_object(struct klp_object *obj);
void klp_unpatch_objects(struct klp_patch *patch);
void klp_unpatch_objects_dynamic(struct klp_patch *patch);

#endif /* _LIVEPATCH_PATCH_H */
