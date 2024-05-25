// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/math64.h>
#include <linux/sched/clock.h>

#include <linux/mailbox/mtk-cmdq-sec-tee.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

/* lock to protect atomic secure task execution */
static DEFINE_MUTEX(cmdq_sec_exec_lock);

void cmdq_sec_setup_tee_context(struct cmdq_sec_tee_context *tee)
{
	/* 09010000 0000 0000 0000000000000000 */
	memset(tee->uuid, 0, sizeof(tee->uuid));
	tee->uuid[0] = 0x9;
	tee->uuid[1] = 0x1;
}
EXPORT_SYMBOL_GPL(cmdq_sec_setup_tee_context);

#if IS_ENABLED(CONFIG_TEE)
static int tee_dev_match(struct tee_ioctl_version_data *t, const void *v)
{
	if (t->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;

	return 0;
}
#endif

int cmdq_sec_init_context(struct cmdq_sec_tee_context *tee)
{
#if IS_ENABLED(CONFIG_TEE)
	tee->tee_context = tee_client_open_context(NULL, tee_dev_match, NULL, NULL);
	if (!tee->tee_context) {
		pr_err("[%s][%d] tee_client_open_context failed!", __func__, __LINE__);
		return -EFAULT;
	}

	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_init_context);

int cmdq_sec_deinit_context(struct cmdq_sec_tee_context *tee)
{
#if IS_ENABLED(CONFIG_TEE)
	if (tee && tee->tee_context)
		tee_client_close_context(tee->tee_context);

	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_deinit_context);

int cmdq_sec_allocate_wsm(struct cmdq_sec_tee_context *tee, void **wsm_buffer, u32 size)
{
#if IS_ENABLED(CONFIG_TEE)
	void *buffer;

	if (!wsm_buffer)
		return -EINVAL;

	if (size == 0)
		return -EINVAL;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	tee->shared_mem = tee_shm_register_kernel_buf(tee->tee_context, buffer, size);
	if (!tee->shared_mem) {
		kfree(buffer);
		return -ENOMEM;
	}

	*wsm_buffer = buffer;

	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_allocate_wsm);

int cmdq_sec_free_wsm(struct cmdq_sec_tee_context *tee, void **wsm_buffer)
{
#if IS_ENABLED(CONFIG_TEE)
	if (!wsm_buffer)
		return -EINVAL;

	tee_shm_free(tee->shared_mem);
	tee->shared_mem = NULL;
	kfree(*wsm_buffer);
	*wsm_buffer = NULL;

	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_free_wsm);

int cmdq_sec_open_session(struct cmdq_sec_tee_context *tee, void *wsm_buffer)
{
#if IS_ENABLED(CONFIG_TEE)
	struct tee_ioctl_open_session_arg osarg = {0};
	struct tee_param params = {0};
	int ret = 0;

	if (!wsm_buffer)
		return -EINVAL;

	osarg.num_params = 1;
	memcpy(osarg.uuid, tee->uuid, sizeof(osarg.uuid));
	osarg.clnt_login = 0;

	ret = tee_client_open_session(tee->tee_context, &osarg, &params);
	if (ret)
		return -EFAULT;

	if (!osarg.ret)
		tee->session = osarg.session;

	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_open_session);

int cmdq_sec_close_session(struct cmdq_sec_tee_context *tee)
{
#if IS_ENABLED(CONFIG_TEE)
	tee_client_close_session(tee->tee_context, tee->session);
	return 0;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_close_session);

int cmdq_sec_execute_session(struct cmdq_sec_tee_context *tee, u32 cmd, s32 timeout_ms)
{
#if IS_ENABLED(CONFIG_TEE)
	struct tee_ioctl_invoke_arg invoke_arg = {0};
	struct tee_param params = {0};
	u64 ts = sched_clock();
	int ret = 0;

	mutex_lock(&cmdq_sec_exec_lock);

	params.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	params.u.memref.shm = tee->shared_mem;
	params.u.memref.shm_offs = 0;
	params.u.memref.size = tee->shared_mem->size;

	invoke_arg.num_params = 1;
	invoke_arg.session = tee->session;
	invoke_arg.func = cmd;

	ret = tee_client_invoke_func(tee->tee_context, &invoke_arg, &params);
	if (ret) {
		pr_err("tee_client_invoke_func failed, ret=%d\n", ret);
		return -EFAULT;
	}

	ret = invoke_arg.ret;

	mutex_unlock(&cmdq_sec_exec_lock);

	ts = div_u64(sched_clock() - ts, 1000000);

	if (ret != 0)
		pr_err("[SEC]execute: TEEC_InvokeCommand:%u ret:%d cost:%lluus", cmd, ret, ts);
	else if (ts > timeout_ms)
		pr_err("[SEC]execute: TEEC_InvokeCommand:%u ret:%d cost:%lluus", cmd, ret, ts);
	else
		pr_debug("[SEC]execute: TEEC_InvokeCommand:%u ret:%d cost:%lluus", cmd, ret, ts);

	return ret;
#else
	return -EFAULT;
#endif
}
EXPORT_SYMBOL_GPL(cmdq_sec_execute_session);

MODULE_LICENSE("GPL");
