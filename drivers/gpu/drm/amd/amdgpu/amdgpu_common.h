/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __AMDGPU_COMMON_H__
#define __AMDGPU_COMMON_H__

uint32_t read_indexed_register(struct amdgpu_device *adev,
			       u32 se_num, u32 sh_num, u32 reg_offset);

void program_aspm(struct amdgpu_device *adev);

int common_sw_init(struct amdgpu_ip_block *ip_block);

#endif
