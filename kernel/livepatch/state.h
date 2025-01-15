/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIVEPATCH_STATE_H
#define _LIVEPATCH_STATE_H

#include <linux/livepatch.h>

bool klp_is_patch_compatible(struct klp_patch *patch);
int klp_states_pre_patch(struct klp_patch *patch);
void klp_states_post_patch(struct klp_patch *patch);
void klp_states_pre_unpatch(struct klp_patch *patch);
void klp_states_post_unpatch(struct klp_patch *patch);

void klp_states_pre_unpatch_replaced(struct klp_patch *patch);
void klp_states_post_unpatch_replaced(struct klp_patch *patch);
void klp_states_post_patch_replaced(struct klp_patch *patch);

#endif /* _LIVEPATCH_STATE_H */
