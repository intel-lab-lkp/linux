// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2023 Intel Corporation. All rights rsvd. */

#include "idxd.h"
#include "iaa_crypto.h"

int iaa_aecs_init_dynamic(void)
{
	int ret;

	ret = add_iaa_compression_mode("dynamic", NULL, 0, NULL, 0, NULL, NULL);

	if (!ret)
		pr_debug("IAA dynamic compression mode initialized\n");

	return ret;
}

void iaa_aecs_cleanup_dynamic(void)
{
	remove_iaa_compression_mode("dynamic");
}
