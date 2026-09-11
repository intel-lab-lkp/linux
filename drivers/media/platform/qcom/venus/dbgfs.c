// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Linaro Ltd.
 */

#include <linux/debugfs.h>
#include <linux/fault-inject.h>
#include <linux/pm_runtime.h>

#include "core.h"

#ifdef CONFIG_FAULT_INJECTION
DECLARE_FAULT_ATTR(venus_ssr_attr);
#endif

static int venus_fw_level_get(void *data, u64 *val)
{
	*val = READ_ONCE(venus_fw_debug);

	return 0;
}

static int venus_fw_level_set(void *data, u64 val)
{
	struct venus_core *core = data;
	bool verbose;
	u32 fw_debug;

	fw_debug = (u32)val;
	verbose = fw_debug & ~(HFI_DEBUG_MSG_ERROR | HFI_DEBUG_MSG_FATAL);
	WRITE_ONCE(venus_fw_debug, fw_debug);

	if (verbose) {
		WRITE_ONCE(core->hw_rsp_timeout, 4 * VENUS_HW_RSP_TIMEOUT_MS);
		WRITE_ONCE(core->pc_delay_timeout, 4 * VENUS_PC_DELAY_TIMEOUT_US);

		if (core->dev_dec)
			pm_runtime_set_autosuspend_delay(core->dev_dec,
							 4 * VENUS_AUTOSUSPEND_DELAY_MS);

		if (core->dev_enc)
			pm_runtime_set_autosuspend_delay(core->dev_enc,
							 4 * VENUS_AUTOSUSPEND_DELAY_MS);
	} else {
		WRITE_ONCE(core->hw_rsp_timeout, VENUS_HW_RSP_TIMEOUT_MS);
		WRITE_ONCE(core->pc_delay_timeout, VENUS_PC_DELAY_TIMEOUT_US);

		if (core->dev_dec)
			pm_runtime_set_autosuspend_delay(core->dev_dec,
							 VENUS_AUTOSUSPEND_DELAY_MS);

		if (core->dev_enc)
			pm_runtime_set_autosuspend_delay(core->dev_enc,
							 VENUS_AUTOSUSPEND_DELAY_MS);
	}

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(venus_fw_level_fops,
			 venus_fw_level_get, venus_fw_level_set, "0x%08llx\n");

void venus_dbgfs_init(struct venus_core *core)
{
	core->root = debugfs_create_dir("venus", NULL);
	debugfs_create_file("fw_level", 0644, core->root, core,
			    &venus_fw_level_fops);

#ifdef CONFIG_FAULT_INJECTION
	fault_create_debugfs_attr("fail_ssr", core->root, &venus_ssr_attr);
#endif
}

void venus_dbgfs_deinit(struct venus_core *core)
{
	debugfs_remove_recursive(core->root);
}
