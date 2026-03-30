// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>

#include "amdgpu.h"
#include "amdgpu_common.h"
#include "mxgpu_nv.h"

uint32_t read_indexed_register(struct amdgpu_device *adev,
			       u32 se_num, u32 sh_num, u32 reg_offset)
{
	uint32_t val;

	mutex_lock(&adev->grbm_idx_mutex);
	if (se_num != 0xffffffff || sh_num != 0xffffffff)
		amdgpu_gfx_select_se_sh(adev, se_num, sh_num, 0xffffffff, 0);

	val = RREG32(reg_offset);

	if (se_num != 0xffffffff || sh_num != 0xffffffff)
		amdgpu_gfx_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
	mutex_unlock(&adev->grbm_idx_mutex);
	return val;
}

void program_aspm(struct amdgpu_device *adev)
{
	if (!amdgpu_device_should_use_aspm(adev))
		return;

	if (adev->nbio.funcs->program_aspm)
		adev->nbio.funcs->program_aspm(adev);
}

int common_sw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;

	if (amdgpu_sriov_vf(adev))
		xgpu_nv_mailbox_add_irq_id(adev);

	return 0;
}
