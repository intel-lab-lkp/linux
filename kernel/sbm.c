// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Petr Tesarik <petr.tesarik1@huawei-partners.com>
 *
 * SandBox Mode (SBM) public API and generic functions.
 */

#include <linux/export.h>
#include <linux/sbm.h>
#include <linux/string.h>

int sbm_init(struct sbm *sbm)
{
	memset(sbm, 0, sizeof(*sbm));

	sbm->error = arch_sbm_init(sbm);
	if (sbm->error)
		return sbm->error;

	return 0;
}
EXPORT_SYMBOL(sbm_init);

void sbm_destroy(struct sbm *sbm)
{
	arch_sbm_destroy(sbm);
}
EXPORT_SYMBOL(sbm_destroy);

int sbm_exec(struct sbm *sbm, sbm_func func, void *args)
{
	int ret;

	if (sbm->error)
		return sbm->error;

	ret = arch_sbm_exec(sbm, func, args);
	if (sbm->error)
		return sbm->error;

	return ret;
}
EXPORT_SYMBOL(sbm_exec);
