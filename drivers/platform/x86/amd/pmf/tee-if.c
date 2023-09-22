// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Platform Management Framework Driver - TEE Interface
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 */

#include <linux/tee_drv.h>
#include <linux/uuid.h>
#include "pmf.h"

#define MAX_TEE_PARAM	4

/* Policy binary actions sampling frequency (in ms) */
static int pb_actions_ms = 1000;
#ifdef CONFIG_AMD_PMF_DEBUG
module_param(pb_actions_ms, int, 0644);
MODULE_PARM_DESC(pb_actions_ms, "Policy binary actions sampling frequency (default = 1000ms)");
#endif

static const uuid_t amd_pmf_ta_uuid = UUID_INIT(0x6fd93b77, 0x3fb8, 0x524d,
						0xb1, 0x2d, 0xc5, 0x29, 0xb1, 0x3d, 0x85, 0x43);

static const char *amd_pmf_uevent_as_str(unsigned int state)
{
	switch (state) {
	case SYSTEM_STATE__S0i3:
		return "S0i3";
	case SYSTEM_STATE__S4:
		return "S4";
	case SYSTEM_STATE__SCREEN_LOCK:
		return "SCREEN_LOCK";
	default:
		return "Unknown Smart PC event";
	}
}

static void amd_pmf_prepare_args(struct amd_pmf_dev *dev, int cmd,
				 struct tee_ioctl_invoke_arg *arg,
				 struct tee_param *param)
{
	memset(arg, 0, sizeof(*arg));
	memset(param, 0, MAX_TEE_PARAM * sizeof(*param));

	arg->func = cmd;
	arg->session = dev->session_id;
	arg->num_params = MAX_TEE_PARAM;

	/* Fill invoke cmd params */
	param[0].u.memref.size = sizeof(struct ta_pmf_shared_memory);
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	param[0].u.memref.shm = dev->fw_shm_pool;
	param[0].u.memref.shm_offs = 0;
}

static int amd_pmf_update_uevents(struct amd_pmf_dev *dev, u16 event)
{
	char *envp[2] = {};

	envp[0] = kasprintf(GFP_KERNEL, "EVENT_ID=%d", event);
	if (!envp[0])
		return -EINVAL;

	kobject_uevent_env(&dev->dev->kobj, KOBJ_CHANGE, envp);

	kfree(envp[0]);
	return 0;
}

static void amd_pmf_apply_policies(struct amd_pmf_dev *dev, struct ta_pmf_enact_result *out)
{
	u32 val, event = 0;
	int idx;

	for (idx = 0; idx < out->actions_count; idx++) {
		val = out->actions_list[idx].value;
		switch (out->actions_list[idx].action_index) {
		case PMF_POLICY_SPL:
			if (dev->prev_data->spl != val) {
				amd_pmf_send_cmd(dev, SET_SPL, false, val, NULL);
				dev_dbg(dev->dev, "update SPL : %d\n", val);
				dev->prev_data->spl = val;
			}
			break;

		case PMF_POLICY_SPPT:
			if (dev->prev_data->sppt != val) {
				amd_pmf_send_cmd(dev, SET_SPPT, false, val, NULL);
				dev_dbg(dev->dev, "update SPPT : %d\n", val);
				dev->prev_data->sppt = val;
			}
			break;

		case PMF_POLICY_FPPT:
			if (dev->prev_data->fppt != val) {
				amd_pmf_send_cmd(dev, SET_FPPT, false, val, NULL);
				dev_dbg(dev->dev, "update FPPT : %d\n", val);
				dev->prev_data->fppt = val;
			}
			break;

		case PMF_POLICY_SPPT_APU_ONLY:
			if (dev->prev_data->sppt_apuonly != val) {
				amd_pmf_send_cmd(dev, SET_SPPT_APU_ONLY, false, val, NULL);
				dev_dbg(dev->dev, "update SPPT_APU_ONLY : %d\n", val);
				dev->prev_data->sppt_apuonly = val;
			}
			break;

		case PMF_POLICY_STT_MIN:
			if (dev->prev_data->stt_minlimit != val) {
				amd_pmf_send_cmd(dev, SET_STT_MIN_LIMIT, false, val, NULL);
				dev_dbg(dev->dev, "update STT_MIN : %d\n", val);
				dev->prev_data->stt_minlimit = val;
			}
			break;

		case PMF_POLICY_STT_SKINTEMP_APU:
			if (dev->prev_data->stt_skintemp_apu != val) {
				amd_pmf_send_cmd(dev, SET_STT_LIMIT_APU, false, val, NULL);
				dev_dbg(dev->dev, "update STT_SKINTEMP_APU : %d\n", val);
				dev->prev_data->stt_skintemp_apu = val;
			}
			break;

		case PMF_POLICY_STT_SKINTEMP_HS2:
			if (dev->prev_data->stt_skintemp_hs2 != val) {
				amd_pmf_send_cmd(dev, SET_STT_LIMIT_HS2, false, val, NULL);
				dev_dbg(dev->dev, "update STT_SKINTEMP_HS2 : %d\n", val);
				dev->prev_data->stt_skintemp_hs2 = val;
			}
			break;

		case PMF_POLICY_P3T:
			if (dev->prev_data->p3t_limit != val) {
				amd_pmf_send_cmd(dev, SET_P3T, false, val, NULL);
				dev_dbg(dev->dev, "update P3T : %d\n", val);
				dev->prev_data->p3t_limit = val;
			}
			break;

		case PMF_POLICY_SYSTEM_STATE:
			event = val + 1;
			if (dev->prev_data->system_state != event) {
				amd_pmf_update_uevents(dev, event);
				dev_dbg(dev->dev, "update SYSTEM_STATE : %s\n",
					amd_pmf_uevent_as_str(event));
				dev->prev_data->system_state = 0;
			}
			break;
		}
	}
}

static int amd_pmf_invoke_cmd_enact(struct amd_pmf_dev *dev)
{
	struct ta_pmf_shared_memory *ta_sm = NULL;
	struct ta_pmf_enact_result *out = NULL;
	struct ta_pmf_enact_table *in = NULL;
	struct tee_param param[MAX_TEE_PARAM];
	struct tee_ioctl_invoke_arg arg;
	int ret = 0;

	if (!dev->tee_ctx)
		return -ENODEV;

	memset(dev->shbuf, 0, dev->policy_sz);
	ta_sm = (struct ta_pmf_shared_memory *)dev->shbuf;
	out = &ta_sm->pmf_output.policy_apply_table;
	in = &ta_sm->pmf_input.enact_table;

	memset(ta_sm, 0, sizeof(struct ta_pmf_shared_memory));
	ta_sm->command_id = TA_PMF_COMMAND_POLICY_BUILDER__ENACT_POLICIES;
	ta_sm->if_version = PMF_TA_IF_VERSION__MAJOR;

	amd_pmf_populate_ta_inputs(dev, in);
	amd_pmf_prepare_args(dev, TA_PMF_COMMAND_POLICY_BUILDER__ENACT_POLICIES, &arg, param);

	ret = tee_client_invoke_func(dev->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(dev->dev, "%s failed TEE err: %x, ret:%x\n", __func__, arg.ret, ret);
		return -EINVAL;
	}

	if (ta_sm->pmf_result == TA_PMF_TYPE__SUCCESS && out->actions_count) {
		amd_pmf_dump_ta_inputs(dev, in);
		dev_dbg(dev->dev, "action count:%d result:%x\n", out->actions_count,
			ta_sm->pmf_result);
		amd_pmf_apply_policies(dev, out);
	}

	return 0;
}

static int amd_pmf_invoke_cmd_init(struct amd_pmf_dev *dev)
{
	struct ta_pmf_shared_memory *ta_sm = NULL;
	struct tee_param param[MAX_TEE_PARAM];
	struct ta_pmf_init_table *in = NULL;
	struct tee_ioctl_invoke_arg arg;
	int ret = 0;

	if (!dev->tee_ctx) {
		dev_err(dev->dev, "%s tee_ctx no context\n", __func__);
		return -ENODEV;
	}

	dev_dbg(dev->dev, "Policy Binary size: %d bytes\n", dev->policy_sz);
	memset(dev->shbuf, 0, dev->policy_sz);
	ta_sm = (struct ta_pmf_shared_memory *)dev->shbuf;
	in = &ta_sm->pmf_input.init_table;

	ta_sm->command_id = TA_PMF_COMMAND_POLICY_BUILDER__INITIALIZE;
	ta_sm->if_version = PMF_TA_IF_VERSION__MAJOR;
	in->metadata_macrocheck = false;
	in->sku_check = false;
	in->validate = true;
	in->frequency = pb_actions_ms;
	in->policies_table.table_size = dev->policy_sz;

	memcpy(in->policies_table.table, dev->policy_buf, dev->policy_sz);
	amd_pmf_prepare_args(dev, TA_PMF_COMMAND_POLICY_BUILDER__INITIALIZE, &arg, param);

	ret = tee_client_invoke_func(dev->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(dev->dev, "%s failed TEE err: %x, ret:%x\n", __func__, arg.ret, ret);
		return -EINVAL;
	}

	return ta_sm->pmf_result;
}

static void amd_pmf_invoke_cmd(struct work_struct *work)
{
	struct amd_pmf_dev *dev = container_of(work, struct amd_pmf_dev, pb_work.work);

	amd_pmf_invoke_cmd_enact(dev);
	schedule_delayed_work(&dev->pb_work, msecs_to_jiffies(pb_actions_ms));
}

static int amd_pmf_start_policy_engine(struct amd_pmf_dev *dev)
{
	u32 cookie, length;
	int res;

	cookie = readl(dev->policy_buf + 0x10);
	length = readl(dev->policy_buf + 0x14);

	if (cookie != POLICY_SIGN_COOKIE || !length)
		return -EINVAL;

	/* update the actual length */
	dev->policy_sz = length + 512;
	res = amd_pmf_invoke_cmd_init(dev);
	if (res == TA_PMF_TYPE__SUCCESS) {
		/* now its safe to announce that smart pc is enabled */
		dev->smart_pc_enabled = 1;
		schedule_delayed_work(&dev->pb_work, msecs_to_jiffies(pb_actions_ms * 3));
	} else {
		dev_err(dev->dev, "%s ta invoke_cmd_init failed err: %x\n", __func__, res);
		return res;
	}

	return 0;
}

static int amd_pmf_get_bios_buffer(struct amd_pmf_dev *dev)
{
	dev->policy_buf = kzalloc(dev->policy_sz, GFP_KERNEL);
	if (!dev->policy_buf)
		return -ENOMEM;

	dev->policy_base = ioremap(dev->policy_addr, dev->policy_sz);
	if (!dev->policy_base)
		return -ENOMEM;

	memcpy(dev->policy_buf, dev->policy_base, dev->policy_sz);

	return amd_pmf_start_policy_engine(dev);
}

static int amd_pmf_amdtee_ta_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return ver->impl_id == TEE_IMPL_ID_AMDTEE;
}

static int amd_pmf_ta_open_session(struct tee_context *ctx, u32 *id)
{
	struct tee_ioctl_open_session_arg sess_arg = {};
	int rc;

	export_uuid(sess_arg.uuid, &amd_pmf_ta_uuid);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;

	rc = tee_client_open_session(ctx, &sess_arg, NULL);
	if (rc < 0 || sess_arg.ret != 0) {
		pr_err("%s: tee_client_open_session failed err:%#x, ret:%#x\n",
		       __func__, sess_arg.ret, rc);
		rc = -EINVAL;
	} else {
		*id = sess_arg.session;
	}

	return rc;
}

static int amd_pmf_tee_init(struct amd_pmf_dev *dev)
{
	int ret;
	u32 size;

	/* Open context with TEE driver */
	dev->tee_ctx = tee_client_open_context(NULL, amd_pmf_amdtee_ta_match, NULL, NULL);
	if (IS_ERR(dev->tee_ctx)) {
		dev_err(dev->dev, "%s: tee_client_open_context failed\n", __func__);
		return PTR_ERR(dev->tee_ctx);
	}

	/* Open session with PMF Trusted App */
	ret = amd_pmf_ta_open_session(dev->tee_ctx, &dev->session_id);
	if (ret) {
		dev_err(dev->dev, "%s: amd_pmf_ta_open_session failed(%d)\n", __func__, ret);
		ret = -EINVAL;
		goto out_ctx;
	}

	size = sizeof(struct ta_pmf_shared_memory) + dev->policy_sz;
	dev->fw_shm_pool = tee_shm_alloc_kernel_buf(dev->tee_ctx, size);
	if (IS_ERR(dev->fw_shm_pool)) {
		dev_err(dev->dev, "%s: tee_shm_alloc_kernel_buf failed\n", __func__);
		ret = PTR_ERR(dev->fw_shm_pool);
		goto out_sess;
	}

	dev->shbuf = tee_shm_get_va(dev->fw_shm_pool, 0);
	dev_dbg(dev->dev, "TEE init done\n");

	return 0;

out_sess:
	tee_client_close_session(dev->tee_ctx, dev->session_id);
out_ctx:
	tee_client_close_context(dev->tee_ctx);

	return ret;
}

static void amd_pmf_tee_deinit(struct amd_pmf_dev *dev)
{
	/* Free the shared memory pool */
	tee_shm_free(dev->fw_shm_pool);

	/* close the existing session with PMF TA*/
	tee_client_close_session(dev->tee_ctx, dev->session_id);

	/* close the context with TEE driver */
	tee_client_close_context(dev->tee_ctx);
}

int amd_pmf_init_smart_pc(struct amd_pmf_dev *dev)
{
	int ret;

	ret = amd_pmf_tee_init(dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&dev->pb_work, amd_pmf_invoke_cmd);
	amd_pmf_set_dram_addr(dev);
	amd_pmf_get_bios_buffer(dev);
	dev->prev_data = kzalloc(sizeof(*dev->prev_data), GFP_KERNEL);
	if (!dev->prev_data)
		return -ENOMEM;

	return dev->smart_pc_enabled;
}

void amd_pmf_deinit_smart_pc(struct amd_pmf_dev *dev)
{
	kfree(dev->prev_data);
	kfree(dev->policy_buf);
	cancel_delayed_work_sync(&dev->pb_work);
	amd_pmf_tee_deinit(dev);
}
