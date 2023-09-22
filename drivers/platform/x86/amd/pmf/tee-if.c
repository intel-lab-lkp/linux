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
static const uuid_t amd_pmf_ta_uuid = UUID_INIT(0x6fd93b77, 0x3fb8, 0x524d,
						0xb1, 0x2d, 0xc5, 0x29, 0xb1, 0x3d, 0x85, 0x43);

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

	size = sizeof(struct ta_pmf_shared_memory);
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
	return amd_pmf_tee_init(dev);
}

void amd_pmf_deinit_smart_pc(struct amd_pmf_dev *dev)
{
	amd_pmf_tee_deinit(dev);
}
