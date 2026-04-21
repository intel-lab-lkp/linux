// SPDX-License-Identifier: GPL-2.0 OR MIT

#include "amdgpu.h"
#include "ivsrcid/vcn/irqsrcs_vcn_5_0.h"
#include "jpeg_v5_0_interrupt.h"

int jpeg_v5_0_process_interrupt_common(struct amdgpu_device *adev,
				       struct amdgpu_iv_entry *entry)
{
	u32 i, inst;

	i = node_id_to_phys_map[entry->node_id];
	DRM_DEV_DEBUG(adev->dev, "IH: JPEG TRAP\n");

	for (inst = 0; inst < adev->jpeg.num_jpeg_inst; ++inst)
		if (adev->jpeg.inst[inst].aid_id == i)
			break;

	if (inst >= adev->jpeg.num_jpeg_inst) {
		dev_WARN_ONCE(adev->dev, 1,
			      "Interrupt received for unknown JPEG instance %d",
			      entry->node_id);
		return 0;
	}

	switch (entry->src_id) {
	case VCN_5_0__SRCID__JPEG_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[0]);
		break;
	case VCN_5_0__SRCID__JPEG1_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[1]);
		break;
	case VCN_5_0__SRCID__JPEG2_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[2]);
		break;
	case VCN_5_0__SRCID__JPEG3_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[3]);
		break;
	case VCN_5_0__SRCID__JPEG4_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[4]);
		break;
	case VCN_5_0__SRCID__JPEG5_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[5]);
		break;
	case VCN_5_0__SRCID__JPEG6_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[6]);
		break;
	case VCN_5_0__SRCID__JPEG7_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[7]);
		break;
	case VCN_5_0__SRCID__JPEG8_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[8]);
		break;
	case VCN_5_0__SRCID__JPEG9_DECODE:
		amdgpu_fence_process(&adev->jpeg.inst[inst].ring_dec[9]);
		break;
	default:
		DRM_DEV_ERROR(adev->dev, "Unhandled interrupt: %d %d\n",
			      entry->src_id, entry->src_data[0]);
		break;
	}

	return 0;
}
