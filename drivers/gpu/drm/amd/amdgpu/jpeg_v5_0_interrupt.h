/* SPDX-License-Identifier: GPL-2.0 OR MIT */

#ifndef __JPEG_V5_0_INTERRUPT_H__
#define __JPEG_V5_0_INTERRUPT_H__

struct amdgpu_device;
struct amdgpu_iv_entry;

int jpeg_v5_0_process_interrupt_common(struct amdgpu_device *adev,
				       struct amdgpu_iv_entry *entry);

#endif /* __JPEG_V5_0_INTERRUPT_H__ */
