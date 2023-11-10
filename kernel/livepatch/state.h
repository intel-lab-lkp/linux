/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LIVEPATCH_STATE_H
#define _LIVEPATCH_STATE_H

#include <linux/livepatch.h>

bool klp_is_patch_compatible(struct klp_patch *patch);
bool klp_patch_disable_blocked(struct klp_patch *patch);
int klp_setup_states(struct klp_patch *patch);
void klp_enable_states(struct klp_patch *patch);
void klp_disable_states(struct klp_patch *patch);
void klp_release_states(struct klp_patch *patch);

void klp_enable_obsolete_states(struct klp_patch *patch);
void klp_disable_obsolete_states(struct klp_patch *patch);
void klp_release_obsolete_states(struct klp_patch *patch);


#endif /* _LIVEPATCH_STATE_H */
