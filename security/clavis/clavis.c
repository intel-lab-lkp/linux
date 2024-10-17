// SPDX-License-Identifier: GPL-2.0
//
#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>
#include "clavis.h"

static struct security_hook_list clavis_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(key_verify_signature, clavis_sig_verify),
};

const struct lsm_id clavis_lsmid = {
	.name = "clavis",
	.id = LSM_ID_CLAVIS,
};

static int __init clavis_lsm_init(void)
{
	clavis_keyring_init();
	security_add_hooks(clavis_hooks, ARRAY_SIZE(clavis_hooks), &clavis_lsmid);
	return 0;
};

DEFINE_LSM(clavis) = {
	.name = "clavis",
	.init = clavis_lsm_init,
};
